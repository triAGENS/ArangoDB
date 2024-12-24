////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "RestHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Auth/TokenCache.h"
#include "Basics/RecursiveLocker.h"
#include "Basics/debugging.h"
#include "Basics/dtrace-wrapper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/LogStructuredParamsAllowList.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Rest/GeneralRequest.h"
#include "Rest/HttpResponse.h"
#include "Scheduler/SchedulerFeature.h"
#include "Statistics/RequestStatistics.h"
#include "Utils/ExecContext.h"
#include "VocBase/Identifiers/TransactionId.h"
#include "VocBase/ticks.h"

#include <absl/strings/str_cat.h>
#include <fuerte/jwt.h>
#include <velocypack/Exception.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestHandler::RestHandler(ArangodServer& server, GeneralRequest* request,
                         GeneralResponse* response)
    : _request(request),
      _response(response),
      _server(server),
      _statistics(),
      _handlerId(0),
      _state(HandlerState::PREPARE),
      _trackedAsOngoingLowPrio(false),
      _lane(RequestLane::UNDEFINED),
      _logContextScopeValues(
          LogContext::makeValue()
              .with<structuredParams::UrlName>(_request->fullUrl())
              .with<structuredParams::UserName>(_request->user())
              .share()),
      _canceled(false) {
  if (server.hasFeature<GeneralServerFeature>() &&
      server.isEnabled<GeneralServerFeature>()) {
    _currentRequestsSizeTracker = metrics::GaugeCounterGuard<std::uint64_t>{
        server.getFeature<GeneralServerFeature>()._currentRequestsSize,
        _request->memoryUsage()};
  }
}

RestHandler::~RestHandler() {
  if (_trackedAsOngoingLowPrio) {
    // someone forgot to call trackTaskEnd 🤔
    TRI_ASSERT(PriorityRequestLane(determineRequestLane()) ==
               RequestPriority::LOW);
    TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
    SchedulerFeature::SCHEDULER->trackEndOngoingLowPriorityTask();
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

void RestHandler::assignHandlerId() {
  _handlerId = TRI_NewServerSpecificTick();
}

uint64_t RestHandler::messageId() const {
  uint64_t messageId = 0UL;
  auto req = _request.get();
  auto res = _response.get();
  if (req) {
    messageId = req->messageId();
  } else if (res) {
    messageId = res->messageId();
  } else {
    LOG_TOPIC("4651e", WARN, Logger::COMMUNICATION)
        << "could not find corresponding request/response";
  }

  return messageId;
}

RequestLane RestHandler::determineRequestLane() {
  if (_lane == RequestLane::UNDEFINED) {
    bool found;
    _request->header(StaticStrings::XArangoFrontend, found);

    if (!found) {
      // header not found, but for requests to root and to /_admin/aardvark/ we
      // are still increasing the priority
      auto const& requestPath = _request->requestPath();
      if (requestPath == "/" || requestPath.starts_with("/_admin/aardvark/")) {
        found = true;
      }
    }

    if (found) {
      _lane = RequestLane::CLIENT_UI;
    } else {
      _lane = lane();

      if (PriorityRequestLane(_lane) == RequestPriority::LOW) {
        // if this is a low-priority request, check if it contains
        // a transaction id, but is not the start of an AQL query
        // or streaming transaction.
        // if we find out that the request is part of an already
        // ongoing transaction, we can now increase its priority,
        // so that ongoing transactions can proceed. however, we
        // don't want to prioritize the start of new transactions
        // here.
        std::string const& value =
            _request->header(StaticStrings::TransactionId, found);

        if (found) {
          TransactionId tid = TransactionId::none();
          std::size_t pos = 0;
          try {
            tid = TransactionId{std::stoull(value, &pos, 10)};
          } catch (...) {
          }
          if (!tid.empty() &&
              !(value.compare(pos, std::string::npos, " aql") == 0 ||
                value.compare(pos, std::string::npos, " begin") == 0)) {
            // increase request priority from previously LOW to now MED.
            _lane = RequestLane::CONTINUATION;
          }
        }
      }
    }
  }
  TRI_ASSERT(_lane != RequestLane::UNDEFINED);
  return _lane;
}

void RestHandler::trackQueueStart() noexcept {
  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
  _statistics.SET_QUEUE_START(
      SchedulerFeature::SCHEDULER->queueStatistics()._queued);
}

void RestHandler::trackQueueEnd() noexcept { _statistics.SET_QUEUE_END(); }

void RestHandler::trackTaskStart() noexcept {
  TRI_ASSERT(!_trackedAsOngoingLowPrio);

  if (PriorityRequestLane(determineRequestLane()) == RequestPriority::LOW) {
    TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
    SchedulerFeature::SCHEDULER->trackBeginOngoingLowPriorityTask();
    _trackedAsOngoingLowPrio = true;
  }
}

void RestHandler::trackTaskEnd() noexcept {
  // the queueing time in seconds
  double queueTime = _statistics.ELAPSED_WHILE_QUEUED();

  if (_trackedAsOngoingLowPrio) {
    TRI_ASSERT(PriorityRequestLane(determineRequestLane()) ==
               RequestPriority::LOW);
    TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
    SchedulerFeature::SCHEDULER->trackEndOngoingLowPriorityTask();
    _trackedAsOngoingLowPrio = false;

    // update the time the last low priority item spent waiting in the queue.

    // the queueing time in ms
    uint64_t queueTimeMs = static_cast<uint64_t>(queueTime * 1000.0);
    SchedulerFeature::SCHEDULER->setLastLowPriorityDequeueTime(queueTimeMs);
  }

  if (queueTime >= 30.0) {
    // this is an informational message about an exceptionally long queuing
    // time. it is not per se a bug, but could be a sign of overload of the
    // instance.
    LOG_TOPIC("e7b15", INFO, Logger::REQUESTS)
        << "request to " << _request->fullUrl() << " was queued for "
        << Logger::FIXED(queueTime) << "s";
  }
}

RequestStatistics::Item&& RestHandler::stealRequestStatistics() {
  return std::move(_statistics);
}

void RestHandler::setRequestStatistics(RequestStatistics::Item&& stat) {
  _statistics = std::move(stat);
}

futures::Future<Result> RestHandler::forwardRequest(bool& forwarded) {
  forwarded = false;
  if (!ServerState::instance()->isCoordinator()) {
    return futures::makeFuture(Result());
  }

  // we must use the request's permissions here and set them in the
  // thread-local variable when calling forwardingTarget().
  // this is because forwardingTarget() may run permission checks.
  ExecContextScope scope(
      basics::downCast<ExecContext>(_request->requestContext()));

  ResultT forwardResult = forwardingTarget();
  if (forwardResult.fail()) {
    return futures::makeFuture(forwardResult.result());
  }

  auto forwardContent = forwardResult.get();
  std::string serverId = std::get<0>(forwardContent);
  bool removeHeader = std::get<1>(forwardContent);

  if (removeHeader) {
    _request->removeHeader(StaticStrings::Authorization);
    _request->setUser("");
  }

  if (serverId.empty()) {
    // no need to actually forward
    return futures::makeFuture(Result());
  }

  NetworkFeature& nf = server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (pool == nullptr) {
    // nullptr happens only during controlled shutdown
    generateError(rest::ResponseCode::SERVICE_UNAVAILABLE,
                  TRI_ERROR_SHUTTING_DOWN, "shutting down server");
    return futures::makeFuture(Result(TRI_ERROR_SHUTTING_DOWN));
  }
  LOG_TOPIC("38d99", DEBUG, Logger::REQUESTS)
      << "forwarding request " << _request->messageId() << " to " << serverId;

  forwarded = true;

  std::string const& dbname = _request->databaseName();

  std::map<std::string, std::string> headers{_request->headers().begin(),
                                             _request->headers().end()};

  // always remove HTTP "Connection" header, so that we don't relay
  // "Connection: Close" or "Connection: Keep-Alive" or such
  headers.erase(StaticStrings::Connection);

  if (headers.find(StaticStrings::Authorization) == headers.end()) {
    // No authorization header is set.
    // In this case, we have to produce a proper JWT token as authorization:
    auto auth = AuthenticationFeature::instance();
    if (auth != nullptr && auth->isActive()) {
      // when in superuser mode, username is empty
      // in this case ClusterComm will add the default superuser token
      std::string const& username = _request->user();
      if (!username.empty()) {
        headers.emplace(
            StaticStrings::Authorization,
            "bearer " + fuerte::jwt::generateUserToken(
                            auth->tokenCache().jwtSecret(), username));
      }
    }
  }

  network::RequestOptions options;
  options.database = dbname;
  options.timeout = network::Timeout(900);

  // if the type is unset JSON is used
  options.contentType = rest::contentTypeToString(_request->contentType());

  options.acceptType =
      rest::contentTypeToString(_request->contentTypeResponse());

  for (auto const& i : _request->values()) {
    options.param(i.first, i.second);
  }

  auto requestType = fuerte::from_string(
      GeneralRequest::translateMethod(_request->requestType()));

  std::string_view resPayload = _request->rawPayload();
  VPackBuffer<uint8_t> payload(resPayload.size());
  payload.append(resPayload.data(), resPayload.size());

  nf.trackForwardedRequest();

  // Should the coordinator be gone by now, we'll respond with 404.
  // There is no point forwarding requests. This affects transactions, cursors,
  // ...
  if (server()
          .getFeature<ClusterFeature>()
          .clusterInfo()
          .getServerEndpoint(serverId)
          .empty()) {
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_CLUSTER_SERVER_UNKNOWN,
                  std::string("cluster server ") + serverId + " unknown");
    return Result(TRI_ERROR_CLUSTER_SERVER_UNKNOWN);
  }

  auto future = network::sendRequestRetry(
      pool, "server:" + serverId, requestType, _request->requestPath(),
      std::move(payload), options, std::move(headers));
  auto cb = [this, serverId, self = shared_from_this()](
                network::Response&& response) -> Result {
    auto res = network::fuerteToArangoErrorCode(response);
    if (res != TRI_ERROR_NO_ERROR) {
      generateError(res);
      return Result(res);
    }

    resetResponse(static_cast<rest::ResponseCode>(response.statusCode()));
    _response->setContentType(
        fuerte::v1::to_string(response.response().contentType()));

    HttpResponse* httpResponse = dynamic_cast<HttpResponse*>(_response.get());
    if (_response == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "invalid response type");
    }
    httpResponse->body() = response.response().payloadAsString();

    auto const& resultHeaders = response.response().messageHeader().meta();
    for (auto const& it : resultHeaders) {
      if (it.first == "http/1.1") {
        // never forward this header, as the HTTP response code was already set
        // via "resetResponse" above
        continue;
      }
      _response->setHeader(it.first, it.second);
    }
    _response->setHeaderNC(StaticStrings::RequestForwardedTo, serverId);

    return Result();
  };
  return std::move(future).thenValue(cb);
}

void RestHandler::handleExceptionPtr(std::exception_ptr eptr) noexcept try {
  auto buildException = [this](ErrorCode code, std::string message,
                               SourceLocation location =
                                   SourceLocation::current()) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("b6302", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: " << message;
#endif
    Exception err(code, std::move(message), location);
    handleError(err);
  };

  try {
    if (eptr) {
      std::rethrow_exception(eptr);
    }
  } catch (Exception const& ex) {
    std::string message =
        absl::StrCat("caught exception in ", name(), ": ", ex.what());
    buildException(ex.code(), std::move(message));
  } catch (velocypack::Exception const& ex) {
    bool const isParseError =
        (ex.errorCode() == arangodb::velocypack::Exception::ParseError ||
         ex.errorCode() ==
             arangodb::velocypack::Exception::UnexpectedControlCharacter);
    std::string message =
        absl::StrCat("caught velocypack error in ", name(), ": ", ex.what());
    buildException(
        isParseError ? TRI_ERROR_HTTP_CORRUPTED_JSON : TRI_ERROR_INTERNAL,
        std::move(message));
  } catch (std::bad_alloc const& ex) {
    std::string message =
        absl::StrCat("caught memory exception in ", name(), ": ", ex.what());
    buildException(TRI_ERROR_OUT_OF_MEMORY, std::move(message));
  } catch (std::exception const& ex) {
    std::string message =
        absl::StrCat("caught exception in ", name(), ": ", ex.what());
    buildException(TRI_ERROR_INTERNAL, std::move(message));
  } catch (...) {
    std::string message = absl::StrCat("caught unknown exception in ", name());
    buildException(TRI_ERROR_INTERNAL, std::move(message));
  }
} catch (...) {
  // we can only get here if putting together an error response or an
  // error log message failed with an exception. there is nothing we
  // can do here to signal this problem.
}

void RestHandler::runHandlerStateMachine() {
  // _executionMutex has to be locked here
  TRI_ASSERT(_sendResponseCallback);

  while (true) {
    switch (_state) {
      case HandlerState::PREPARE:
        prepareEngine();
        break;

      case HandlerState::EXECUTE: {
        executeEngine(/*isContinue*/ false);
        if (_state == HandlerState::PAUSED) {
          shutdownExecute(false);
          LOG_TOPIC("23a33", DEBUG, Logger::COMMUNICATION)
              << "Pausing rest handler execution " << this;
          return;  // stop state machine
        }
        break;
      }

      case HandlerState::CONTINUED: {
        executeEngine(/*isContinue*/ true);
        if (_state == HandlerState::PAUSED) {
          shutdownExecute(/*isFinalized*/ false);
          LOG_TOPIC("23727", DEBUG, Logger::COMMUNICATION)
              << "Pausing rest handler execution " << this;
          return;  // stop state machine
        }
        break;
      }

      case HandlerState::PAUSED:
        LOG_TOPIC("ae26f", DEBUG, Logger::COMMUNICATION)
            << "Resuming rest handler execution " << this;
        _state = HandlerState::CONTINUED;
        break;

      case HandlerState::FINALIZE:
        _statistics.SET_REQUEST_END();

        // shutdownExecute is noexcept
        shutdownExecute(true);  // may not be moved down

        _state = HandlerState::DONE;

        // compress response if required
        compressResponse();
        // Callback may stealStatistics!
        _sendResponseCallback(this);
        break;

      case HandlerState::FAILED:
        _statistics.SET_REQUEST_END();
        // Callback may stealStatistics!
        _sendResponseCallback(this);

        shutdownExecute(false);
        return;

      case HandlerState::DONE:
        return;
    }
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

void RestHandler::prepareEngine() {
  // set end immediately so we do not get negative statistics
  _statistics.SET_REQUEST_START_END();

  if (_canceled) {
    _state = HandlerState::FAILED;

    Exception err(TRI_ERROR_REQUEST_CANCELED);
    handleError(err);
    return;
  }

  try {
    prepareExecute(false);
    _state = HandlerState::EXECUTE;
    return;
  } catch (Exception const& ex) {
    handleError(ex);
  } catch (std::exception const& ex) {
    Exception err(TRI_ERROR_INTERNAL, ex.what());
    handleError(err);
  } catch (...) {
    Exception err(TRI_ERROR_INTERNAL);
    handleError(err);
  }

  _state = HandlerState::FAILED;
}

void RestHandler::prepareExecute(bool isContinue) {
  _logContextEntry = LogContext::Current::pushValues(_logContextScopeValues);
}

void RestHandler::shutdownExecute(bool isFinalized) noexcept {
  LogContext::Current::popEntry(_logContextEntry);
}

/// Execute the rest handler state machine. Retry the wakeup,
/// returns true if _state == PAUSED, false otherwise
bool RestHandler::wakeupHandler() {
  auto guard = std::unique_lock(_executionMutex);
  _lockAdopted = false;
  if (_state == HandlerState::PAUSED) {
    runHandlerStateMachine();
  }
  if (_lockAdopted) {
    guard.release();
  }
  return _state == HandlerState::PAUSED;
}

void RestHandler::executeEngine(bool isContinue) {
  DTRACE_PROBE1(arangod, RestHandlerExecuteEngine, this);
  ExecContextScope scope(
      basics::downCast<ExecContext>(_request->requestContext()));

  try {
    RestStatus result = RestStatus::DONE;
    if (isContinue) {
      // only need to run prepareExecute() again when we are continuing
      // otherwise prepareExecute() was already run in the PREPARE phase
      prepareExecute(true);
      result = continueExecute();
    } else {
      result = execute();
    }

    if (result == RestStatus::WAITING) {
      _state = HandlerState::PAUSED;  // wait for someone to continue the state
                                      // machine
      return;
    }

    if (_response == nullptr) {
      Exception err(TRI_ERROR_INTERNAL, "no response received from handler");
      handleError(err);
    }

    _state = HandlerState::FINALIZE;
    return;
  } catch (Exception const& ex) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("11928", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: caught exception in " << name() << ": "
        << ex.what();
#endif
    handleError(ex);
  } catch (arangodb::velocypack::Exception const& ex) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("fdcbb", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: caught velocypack exception in " << name() << ": "
        << ex.what();
#endif
    bool const isParseError =
        (ex.errorCode() == arangodb::velocypack::Exception::ParseError ||
         ex.errorCode() ==
             arangodb::velocypack::Exception::UnexpectedControlCharacter);
    Exception err(
        isParseError ? TRI_ERROR_HTTP_CORRUPTED_JSON : TRI_ERROR_INTERNAL,
        std::string("VPack error: ") + ex.what());
    handleError(err);
  } catch (std::bad_alloc const& ex) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("5c9f5", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: caught memory exception in " << name() << ": "
        << ex.what();
#endif
    Exception err(TRI_ERROR_OUT_OF_MEMORY, ex.what());
    handleError(err);
  } catch (std::exception const& ex) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("252e9", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: caught exception in " << name() << ": "
        << ex.what();
#endif
    Exception err(TRI_ERROR_INTERNAL, ex.what());
    handleError(err);
  } catch (...) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("f729c", WARN, arangodb::Logger::FIXME)
        << "maintainer mode: caught unknown exception in " << name();
#endif
    Exception err(TRI_ERROR_INTERNAL);
    handleError(err);
  }

  _state = HandlerState::FAILED;
}

void RestHandler::generateError(rest::ResponseCode code, ErrorCode errorNumber,
                                std::string_view const errorMessage) {
  resetResponse(code);

  if (_request->requestType() != rest::RequestType::HEAD) {
    VPackBuffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    try {
      builder.add(VPackValue(VPackValueType::Object));
      builder.add(StaticStrings::Code, VPackValue(static_cast<int>(code)));
      builder.add(StaticStrings::Error, VPackValue(true));
      builder.add(StaticStrings::ErrorMessage, VPackValue(errorMessage));
      builder.add(StaticStrings::ErrorNum, VPackValue(errorNumber));
      builder.close();

      if (_request != nullptr) {
        _response->setContentType(_request->contentTypeResponse());
      }
      _response->setPayload(std::move(buffer), VPackOptions::Defaults,
                            /*resolveExternals*/ false);
    } catch (...) {
      // exception while generating error
    }
  }
}

void RestHandler::compressResponse() {
  if (_isAsyncRequest) {
    // responses to async requests are currently not compressed
    return;
  }

  rest::ResponseCompressionType rct = _response->compressionAllowed();
  if (rct == rest::ResponseCompressionType::kNoCompression) {
    // compression explicitly disabled for the response
    return;
  }

  if (_request->acceptEncoding() == rest::EncodingType::UNSET) {
    // client hasn't asked for compression
    return;
  }

  size_t bodySize = _response->bodySize();
  if (bodySize == 0) {
    // response body size of 0 does not need any compression
    return;
  }

  uint64_t threshold =
      server().getFeature<GeneralServerFeature>().compressResponseThreshold();

  if (threshold == 0) {
    // opted out of compression by configuration
    return;
  }

  // check if response is eligible for compression
  if (bodySize < threshold) {
    // compression not necessary
    return;
  }

  if (_response->headers().contains(StaticStrings::ContentEncoding)) {
    // response is already content-encoded
    return;
  }

  TRI_ASSERT(bodySize > 0);
  TRI_ASSERT(_request->acceptEncoding() != rest::EncodingType::UNSET);

  switch (_request->acceptEncoding()) {
    case rest::EncodingType::DEFLATE:
      // the resulting compressed response body may be larger than the
      // uncompressed input size. in this case we are not returning the
      // compressed response body, but the original, uncompressed body.
      if (_response->zlibDeflate(/*onlyIfSmaller*/ true) ==
          TRI_ERROR_NO_ERROR) {
        _response->setHeaderNC(StaticStrings::ContentEncoding,
                               StaticStrings::EncodingDeflate);
      }
      break;

    case rest::EncodingType::GZIP:
      // the resulting compressed response body may be larger than the
      // uncompressed input size. in this case we are not returning the
      // compressed response body, but the original, uncompressed body.
      if (_response->gzipCompress(/*onlyIfSmaller*/ true) ==
          TRI_ERROR_NO_ERROR) {
        _response->setHeaderNC(StaticStrings::ContentEncoding,
                               StaticStrings::EncodingGzip);
      }
      break;

    case rest::EncodingType::LZ4:
      // the resulting compressed response body may be larger than the
      // uncompressed input size. in this case we are not returning the
      // compressed response body, but the original, uncompressed body.
      if (_response->lz4Compress(/*onlyIfSmaller*/ true) ==
          TRI_ERROR_NO_ERROR) {
        _response->setHeaderNC(StaticStrings::ContentEncoding,
                               StaticStrings::EncodingArangoLz4);
      }
      break;

    default:
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates an error
////////////////////////////////////////////////////////////////////////////////

void RestHandler::generateError(rest::ResponseCode code,
                                ErrorCode errorNumber) {
  auto const message = TRI_errno_string(errorNumber);

  if (message.data() != nullptr) {
    generateError(code, errorNumber, message);
  } else {
    generateError(code, errorNumber, "unknown error");
  }
}

// generates an error
void RestHandler::generateError(arangodb::Result const& r) {
  ResponseCode code = GeneralResponse::responseCode(r.errorNumber());
  generateError(code, r.errorNumber(), r.errorMessage());
}

RestStatus RestHandler::waitForFuture(futures::Future<futures::Unit>&& f) {
  return waitForFuture(
      std::move(f).thenValue([](futures::Unit) { return RestStatus::DONE; }));
}

RestStatus RestHandler::waitForFuture(futures::Future<RestStatus>&& f) {
  if (f.isReady()) {             // fast-path out
    f.result().throwIfFailed();  // just throw the error upwards
    return f.waitAndGet();
  }
  TRI_ASSERT(!_lockAdopted);
  // We adopt the _executionMutex. Setting this informs the function that locked
  // it (either runHandler or wakeupHandler) about that.
  _lockAdopted = true;
  TRI_ASSERT(_executionCounter == 0);
  _executionCounter = 2;
  std::move(f).thenFinal(
      withLogContext([self = shared_from_this()](
                         futures::Try<RestStatus>&& t) noexcept -> void {
        auto guard = std::unique_lock(self->_executionMutex, std::adopt_lock);
        if (t.hasException()) {
          self->handleExceptionPtr(std::move(t).exception());
          self->_followupRestStatus = RestStatus::DONE;
        } else {
          self->_followupRestStatus = t.get();
          if (t.get() == RestStatus::WAITING) {
            return;  // rest handler will be woken up externally
          }
        }
        if (--self->_executionCounter == 0) {
          guard.unlock();
          self->wakeupHandler();
        }
      }));
  return --_executionCounter == 0 ? _followupRestStatus : RestStatus::WAITING;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 protected methods
// -----------------------------------------------------------------------------

void RestHandler::resetResponse(rest::ResponseCode code) {
  TRI_ASSERT(_response != nullptr);
  _response->reset(code);
}

futures::Future<futures::Unit> RestHandler::executeAsync() {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

RestStatus RestHandler::execute() { return waitForFuture(executeAsync()); }

void RestHandler::runHandler(
    std::function<void(rest::RestHandler*)> responseCallback) {
  TRI_ASSERT(_state == HandlerState::PREPARE);
  _sendResponseCallback = std::move(responseCallback);
  auto guard = std::unique_lock(_executionMutex);
  _lockAdopted = false;
  runHandlerStateMachine();
  if (_lockAdopted) {
    guard.release();
  }
}
