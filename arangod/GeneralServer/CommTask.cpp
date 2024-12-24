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
/// @author Achim Brandt
/// @author Dr. Frank Celler
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "CommTask.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Auth/UserManager.h"
#include "Basics/EncodingUtils.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/dtrace-wrapper.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AsyncJobManager.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/RestHandler.h"
#include "Logger/LogMacros.h"
#include "Replication/ReplicationFeature.h"
#include "Rest/GeneralResponse.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/VocbaseContext.h"
#include "Scheduler/SchedulerFeature.h"
#include "Scheduler/Scheduler.h"
#include "Statistics/ConnectionStatistics.h"
#include "Statistics/RequestStatistics.h"
#include "Utils/Events.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"
#ifdef USE_V8
#include "V8Server/FoxxFeature.h"
#endif

#include <string_view>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

namespace {
// some static URL path prefixes
constexpr std::string_view pathPrefixApi("/_api/");
constexpr std::string_view pathPrefixApiUser("/_api/user/");
constexpr std::string_view pathPrefixAdmin("/_admin/");
constexpr std::string_view pathPrefixAdminAardvark("/_admin/aardvark/");
constexpr std::string_view pathPrefixOpen("/_open/");

VocbasePtr lookupDatabaseFromRequest(ArangodServer& server,
                                     GeneralRequest& req) {
  // get database name from request
  if (req.databaseName().empty()) {
    // if no database name was specified in the request, use system database
    // name as a fallback
    req.setDatabaseName(StaticStrings::SystemDatabase);
  }

  DatabaseFeature& databaseFeature = server.getFeature<DatabaseFeature>();
  return databaseFeature.useDatabase(req.databaseName());
}

/// Set the appropriate requestContext
bool resolveRequestContext(ArangodServer& server, GeneralRequest& req) {
  auto vocbase = lookupDatabaseFromRequest(server, req);

  // invalid database name specified, database not found etc.
  if (vocbase == nullptr) {
    return false;
  }

  TRI_ASSERT(!vocbase->isDangling());

  // FIXME(gnusi): modify VocbaseContext to accept VocbasePtr
  auto context = VocbaseContext::create(req, *vocbase.release());
  if (!context) {
    return false;
  }
  // the VocbaseContext is now responsible for releasing the vocbase
  req.setRequestContext(std::move(context));

  // the "true" means the request is the owner of the context
  return true;
}

bool queueTimeViolated(GeneralRequest const& req) {
  // check if the client sent the "x-arango-queue-time-seconds" header
  bool found = false;
  std::string const& queueTimeValue =
      req.header(StaticStrings::XArangoQueueTimeSeconds, found);
  if (found) {
    // yes, now parse the sent time value. if the value sent by client cannot be
    // parsed as a double, then it will be handled as if "0.0" was sent - i.e.
    // no queuing time restriction
    double requestedQueueTime = StringUtils::doubleDecimal(queueTimeValue);
    if (requestedQueueTime > 0.0) {
      TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
      // value is > 0.0, so now check the last dequeue time that the scheduler
      // reported
      double lastDequeueTime =
          static_cast<double>(
              SchedulerFeature::SCHEDULER->getLastLowPriorityDequeueTime()) /
          1000.0;

      if (lastDequeueTime > requestedQueueTime) {
        // the log topic should actually be REQUESTS here, but the default log
        // level for the REQUESTS log topic is FATAL, so if we logged here in
        // INFO level, it would effectively be suppressed. thus we are using the
        // Scheduler's log topic here, which is somewhat related.
        SchedulerFeature::SCHEDULER->trackQueueTimeViolation();
        LOG_TOPIC("1bbcc", WARN, Logger::THREADS)
            << "dropping incoming request because the client-specified maximum "
               "queue time requirement ("
            << requestedQueueTime
            << "s) would be violated by current queue time (" << lastDequeueTime
            << "s)";
        return true;
      }
    }
  }
  return false;
}

}  // namespace

CommTask::CommTask(GeneralServer& server, ConnectionInfo info)
    : _server(server),
      _generalServerFeature(server.server().getFeature<GeneralServerFeature>()),
      _connectionInfo(std::move(info)),
      _connectionStatistics(acquireConnectionStatistics()),
      _auth(AuthenticationFeature::instance()),
      _isUserRequest(true) {
  TRI_ASSERT(_auth != nullptr);
  _connectionStatistics.SET_START();
}

CommTask::~CommTask() { _connectionStatistics.SET_END(); }

/// Must be called before calling executeRequest, will send an error
/// response if execution is supposed to be aborted
CommTask::Flow CommTask::prepareExecution(
    auth::TokenCache::Entry const& authToken, GeneralRequest& req,
    ServerState::Mode mode) {
  DTRACE_PROBE1(arangod, CommTaskPrepareExecution, this);

  // Step 1: In the shutdown phase we simply return 503:
  if (_server.server().isStopping()) {
    this->sendErrorResponse(ResponseCode::SERVICE_UNAVAILABLE,
                            req.contentTypeResponse(), req.messageId(),
                            TRI_ERROR_SHUTTING_DOWN);
    return Flow::Abort;
  }

  _requestSource = req.header(StaticStrings::ClusterCommSource);
  LOG_TOPIC_IF("e5db9", DEBUG, Logger::REQUESTS, !_requestSource.empty())
      << "\"request-source\",\"" << (void*)this << "\",\"" << _requestSource
      << "\"";

  _isUserRequest = std::invoke([&]() {
    auto role = ServerState::instance()->getRole();
    if (ServerState::isSingleServer(role)) {
      // single server is always user-facing
      return true;
    }
    if (ServerState::isAgent(role) || ServerState::isDBServer(role)) {
      // agents and DB servers are never user-facing
      return false;
    }

    TRI_ASSERT(ServerState::isCoordinator(role));
    // coordinators are only user-facing if the request is not a
    // cluster-internal request
    return !ServerState::isCoordinatorId(_requestSource) &&
           !ServerState::isDBServerId(_requestSource);
  });

  // Step 2: Handle server-modes, i.e. bootstrap / DC2DC stunts
  std::string const& path = req.requestPath();

  bool allowEarlyConnections = _server.allowEarlyConnections();

  switch (mode) {
    case ServerState::Mode::STARTUP: {
      if (!allowEarlyConnections ||
          (_auth->isActive() && !req.authenticated())) {
        if (req.authenticationMethod() == rest::AuthenticationMethod::BASIC) {
          // HTTP basic authentication is not supported during the startup
          // phase, as we do not have any access to the database data. However,
          // we must return HTTP 503 because we cannot even verify the
          // credentials, and let the caller can try again later when the
          // authentication may be available.
          sendErrorResponse(ResponseCode::SERVICE_UNAVAILABLE,
                            req.contentTypeResponse(), req.messageId(),
                            TRI_ERROR_HTTP_SERVICE_UNAVAILABLE,
                            "service unavailable due to startup");
        } else {
          sendErrorResponse(rest::ResponseCode::UNAUTHORIZED,
                            req.contentTypeResponse(), req.messageId(),
                            TRI_ERROR_FORBIDDEN);
        }
        return Flow::Abort;
      }

      // passed authentication!
      TRI_ASSERT(allowEarlyConnections);
      if (path == "/_api/version" || path == "/_admin/version" ||
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
          path.starts_with("/_admin/debug/") ||
#endif
          path == "/_admin/status") {
        return Flow::Continue;
      }
      // most routes are disallowed during startup, except the ones above.
      sendErrorResponse(ResponseCode::SERVICE_UNAVAILABLE,
                        req.contentTypeResponse(), req.messageId(),
                        TRI_ERROR_HTTP_SERVICE_UNAVAILABLE,
                        "service unavailable due to startup");
      return Flow::Abort;
    }

    case ServerState::Mode::MAINTENANCE: {
      if (allowEarlyConnections &&
          (path == "/_api/version" || path == "/_admin/version" ||
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
           path.starts_with("/_admin/debug/") ||
#endif
           path == "/_admin/status")) {
        return Flow::Continue;
      }

      // In the bootstrap phase, we would like that coordinators answer the
      // following endpoints, but not yet others:
      if ((!ServerState::instance()->isCoordinator() &&
           !path.starts_with("/_api/agency/agency-callbacks")) ||
          (!path.starts_with("/_api/agency/agency-callbacks") &&
           !path.starts_with("/_api/aql"))) {
        LOG_TOPIC("63f47", TRACE, arangodb::Logger::FIXME)
            << "Maintenance mode: refused path: " << path;
        sendErrorResponse(
            ResponseCode::SERVICE_UNAVAILABLE, req.contentTypeResponse(),
            req.messageId(), TRI_ERROR_HTTP_SERVICE_UNAVAILABLE,
            "service unavailable due to startup or maintenance mode");
        return Flow::Abort;
      }
      break;
    }
    case ServerState::Mode::DEFAULT:
    case ServerState::Mode::INVALID:
      // no special handling required
      break;
  }

  // Step 3: Try to resolve vocbase and use
  if (!::resolveRequestContext(_server.server(),
                               req)) {  // false if db not found
    if (_auth->isActive()) {
      // prevent guessing database names (issue #5030)
      auth::Level lvl = auth::Level::NONE;
      if (req.authenticated()) {
        // If we are authenticated and the user name is empty, then we must
        // have been authenticated with a superuser JWT token. In this case,
        // we must not check the databaseAuthLevel here.
        if (_auth->userManager() != nullptr && !req.user().empty()) {
          lvl = _auth->userManager()->databaseAuthLevel(req.user(),
                                                        req.databaseName());
        } else {
          lvl = auth::Level::RW;
        }
      }
      if (lvl == auth::Level::NONE) {
        sendErrorResponse(rest::ResponseCode::UNAUTHORIZED,
                          req.contentTypeResponse(), req.messageId(),
                          TRI_ERROR_FORBIDDEN,
                          "not authorized to execute this request");
        return Flow::Abort;
      }
    }
    sendErrorResponse(rest::ResponseCode::NOT_FOUND, req.contentTypeResponse(),
                      req.messageId(), TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    return Flow::Abort;
  }
  TRI_ASSERT(req.requestContext() != nullptr);

  // Step 4: Check the authentication. Will determine if the user can access
  // this path checks db permissions and contains exceptions for the
  // users API to allow logins
  if (CommTask::canAccessPath(authToken, req) != Flow::Continue) {
    events::NotAuthorized(req);
    sendErrorResponse(rest::ResponseCode::UNAUTHORIZED,
                      req.contentTypeResponse(), req.messageId(),
                      TRI_ERROR_FORBIDDEN,
                      "not authorized to execute this request");
    return Flow::Abort;
  }

  if (ServerState::instance()->isSingleServerOrCoordinator()) {
#ifdef USE_V8
    auto& ff = _server.server().getFeature<FoxxFeature>();
    bool foxxEnabled = ff.foxxEnabled();
#else
    constexpr bool foxxEnabled = false;
#endif
    if (!foxxEnabled && !(path == "/" || path.starts_with(::pathPrefixAdmin) ||
                          path.starts_with(::pathPrefixApi) ||
                          path.starts_with(::pathPrefixOpen))) {
      sendErrorResponse(rest::ResponseCode::FORBIDDEN,
                        req.contentTypeResponse(), req.messageId(),
                        TRI_ERROR_FORBIDDEN,
                        "access to Foxx apps is turned off on this instance");
      return Flow::Abort;
    }
  }

  // Step 5: Update global HLC timestamp from authenticated requests
  if (req.authenticated()) {  // TODO only from superuser ??
    // check for an HLC time stamp only with auth
    bool found;
    std::string const& timeStamp = req.header(StaticStrings::HLCHeader, found);
    if (found) {
      uint64_t parsed = basics::HybridLogicalClock::decodeTimeStamp(timeStamp);
      if (parsed != 0 && parsed != UINT64_MAX) {
        TRI_HybridLogicalClock(parsed);
      }
    }
  }

  return Flow::Continue;
}

/// Must be called from sendResponse, before response is rendered
void CommTask::finishExecution(GeneralResponse& res,
                               std::string const& origin) const {
  if (this->_isUserRequest) {
    // CORS response handling - only needed on user facing coordinators
    // or single servers
    if (!origin.empty()) {
      // the request contained an Origin header. We have to send back the
      // access-control-allow-origin header now
      LOG_TOPIC("be603", DEBUG, arangodb::Logger::REQUESTS)
          << "handling CORS response for origin '" << origin << "'";

      // send back original value of "Origin" header
      res.setHeaderNCIfNotSet(StaticStrings::AccessControlAllowOrigin, origin);

      // send back "Access-Control-Allow-Credentials" header
      res.setHeaderNCIfNotSet(
          StaticStrings::AccessControlAllowCredentials,
          (allowCorsCredentials(origin) ? "true" : "false"));

      // use "IfNotSet" here because we should not override HTTP headers set
      // by Foxx applications
      res.setHeaderNCIfNotSet(StaticStrings::AccessControlExposeHeaders,
                              StaticStrings::ExposedCorsHeaders);
    }

    // DB server is not user-facing, and does not need to set this header
    // use "IfNotSet" to not overwrite an existing response header
    res.setHeaderNCIfNotSet(StaticStrings::XContentTypeOptions,
                            StaticStrings::NoSniff);

    // CSP Headers for security.
    res.setHeaderNCIfNotSet(StaticStrings::ContentSecurityPolicy,
                            "frame-ancestors 'self'; form-action 'self';");
    res.setHeaderNCIfNotSet(
        StaticStrings::CacheControl,
        "no-cache, no-store, must-revalidate, pre-check=0, post-check=0, "
        "max-age=0, s-maxage=0");
    res.setHeaderNCIfNotSet(StaticStrings::Pragma, "no-cache");
    res.setHeaderNCIfNotSet(StaticStrings::Expires, "0");
    res.setHeaderNCIfNotSet(StaticStrings::HSTS,
                            "max-age=31536000 ; includeSubDomains");

    // add "x-arango-queue-time-seconds" header
    if (_generalServerFeature.returnQueueTimeHeader()) {
      TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
      res.setHeaderNC(
          StaticStrings::XArangoQueueTimeSeconds,
          std::to_string(
              static_cast<double>(SchedulerFeature::SCHEDULER
                                      ->getLastLowPriorityDequeueTime()) /
              1000.0));
    }
  }
}

/// Push this request into the execution pipeline
void CommTask::executeRequest(std::unique_ptr<GeneralRequest> request,
                              std::unique_ptr<GeneralResponse> response,
                              ServerState::Mode mode) {
  TRI_ASSERT(request != nullptr);
  TRI_ASSERT(response != nullptr);

  if (request == nullptr || response == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid object setup for executeRequest");
  }

  DTRACE_PROBE1(arangod, CommTaskExecuteRequest, this);

  response->setContentTypeRequested(request->contentTypeResponse());
  response->setGenerateBody(request->requestType() != RequestType::HEAD);

  // store the message id for error handling
  uint64_t messageId = request->messageId();

  rest::ContentType const respType = request->contentTypeResponse();

  // check if "x-arango-queue-time-seconds" header was set, and its value
  // is above the current dequeing time
  if (::queueTimeViolated(*request)) {
    sendErrorResponse(rest::ResponseCode::PRECONDITION_FAILED, respType,
                      messageId, TRI_ERROR_QUEUE_TIME_REQUIREMENT_VIOLATED);
    return;
  }

  bool found;
  // check for an async request (before the handler steals the request)
  std::string const& asyncExec = request->header(StaticStrings::Async, found);

  // create a handler, this takes ownership of request and response
  auto& server = _server.server();
  auto factory = _generalServerFeature.handlerFactory();
  auto handler =
      factory->createHandler(server, std::move(request), std::move(response));

  // give up, if we cannot find a handler
  if (handler == nullptr) {
    LOG_TOPIC("90d3a", TRACE, arangodb::Logger::FIXME)
        << "no handler is known, giving up";
    sendSimpleResponse(rest::ResponseCode::NOT_FOUND, respType, messageId,
                       VPackBuffer<uint8_t>());
    return;
  }

  if (mode == ServerState::Mode::STARTUP) {
    // request during startup phase
    handler->setRequestStatistics(stealRequestStatistics(messageId));
    handleRequestStartup(std::move(handler));
    return;
  }

  // forward to correct server if necessary
  bool forwarded;
  auto res = handler->forwardRequest(forwarded);
  if (forwarded) {
    requestStatistics(messageId).SET_SUPERUSER();
    std::move(res).thenFinal(
        [self(shared_from_this()), h(std::move(handler)),
         messageId](futures::Try<Result>&& /*ignored*/) -> void {
          self->sendResponse(h->stealResponse(),
                             self->stealRequestStatistics(messageId));
        });
    return;
  }

  if (res.hasValue() && res.waitAndGet().fail()) {
    auto& r = res.waitAndGet();
    sendErrorResponse(GeneralResponse::responseCode(r.errorNumber()), respType,
                      messageId, r.errorNumber(), r.errorMessage());
    return;
  }

  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
  SchedulerFeature::SCHEDULER->trackCreateHandlerTask();

  // asynchronous request
  if (found && (asyncExec == "true" || asyncExec == "store")) {
    RequestStatistics::Item stats = stealRequestStatistics(messageId);
    stats.SET_ASYNC();
    handler->setRequestStatistics(std::move(stats));
    handler->setIsAsyncRequest();

    uint64_t jobId = 0;

    bool ok = false;
    if (asyncExec == "store") {
      // persist the responses
      ok = handleRequestAsync(std::move(handler), &jobId);
    } else {
      // don't persist the responses
      ok = handleRequestAsync(std::move(handler));
    }

    TRI_IF_FAILURE("queueFull") {
      ok = false;
      jobId = 0;
    }

    if (ok) {
      // always return HTTP 202 Accepted
      auto resp = createResponse(rest::ResponseCode::ACCEPTED, messageId);
      if (jobId > 0) {
        // return the job id we just created
        resp->setHeaderNC(StaticStrings::AsyncId, StringUtils::itoa(jobId));
      }
      sendResponse(std::move(resp), RequestStatistics::Item());
    } else {
      sendErrorResponse(rest::ResponseCode::SERVICE_UNAVAILABLE, respType,
                        messageId, TRI_ERROR_QUEUE_FULL);
    }
  } else {
    // synchronous request
    handler->setRequestStatistics(stealRequestStatistics(messageId));
    // handleRequestSync adds an error response
    handleRequestSync(std::move(handler));
  }
}

// -----------------------------------------------------------------------------
// --SECTION-- statistics handling                             protected methods
// -----------------------------------------------------------------------------

void CommTask::setStatistics(uint64_t id, RequestStatistics::Item&& stat) {
  std::lock_guard guard{_statisticsMutex};
  _statisticsMap.insert_or_assign(id, std::move(stat));
}

ConnectionStatistics::Item CommTask::acquireConnectionStatistics() {
  ConnectionStatistics::Item stat;
  if (_server.server().getFeature<StatisticsFeature>().isEnabled()) {
    // only acquire a new item if the statistics are enabled.
    stat = ConnectionStatistics::acquire();
  }
  return stat;
}

RequestStatistics::Item const& CommTask::acquireRequestStatistics(uint64_t id) {
  RequestStatistics::Item stat;
  if (_server.server().getFeature<StatisticsFeature>().isEnabled()) {
    // only acquire a new item if the statistics are enabled.
    stat = RequestStatistics::acquire();
  }

  std::lock_guard<std::mutex> guard(_statisticsMutex);
  return _statisticsMap.insert_or_assign(id, std::move(stat)).first->second;
}

RequestStatistics::Item const& CommTask::requestStatistics(uint64_t id) {
  std::lock_guard<std::mutex> guard(_statisticsMutex);
  return _statisticsMap[id];
}

RequestStatistics::Item CommTask::stealRequestStatistics(uint64_t id) {
  RequestStatistics::Item result;
  std::lock_guard<std::mutex> guard(_statisticsMutex);

  auto iter = _statisticsMap.find(id);
  if (iter != _statisticsMap.end()) {
    result = std::move(iter->second);
    _statisticsMap.erase(iter);
  }

  return result;
}

/// @brief send error response including response body
void CommTask::sendSimpleResponse(rest::ResponseCode code,
                                  rest::ContentType respType, uint64_t mid,
                                  velocypack::Buffer<uint8_t>&& buffer) {
  try {
    auto resp = createResponse(code, mid);
    resp->setContentType(respType);
    if (!buffer.empty()) {
      resp->setPayload(std::move(buffer), VPackOptions::Defaults);
    }
    sendResponse(std::move(resp), this->stealRequestStatistics(mid));
  } catch (...) {
    LOG_TOPIC("fc831", WARN, Logger::REQUESTS)
        << "addSimpleResponse received an exception, closing connection";
    stop();
  }
}

/// @brief send response including error response body
void CommTask::sendErrorResponse(rest::ResponseCode code,
                                 rest::ContentType respType, uint64_t messageId,
                                 ErrorCode errorNum,
                                 std::string_view errorMessage /* = {} */) {
  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  builder.openObject();
  builder.add(StaticStrings::Error, VPackValue(errorNum != TRI_ERROR_NO_ERROR));
  builder.add(StaticStrings::ErrorNum, VPackValue(errorNum));
  if (errorNum != TRI_ERROR_NO_ERROR) {
    if (errorMessage.data() == nullptr) {
      errorMessage = TRI_errno_string(errorNum);
    }
    TRI_ASSERT(errorMessage.data() != nullptr);
    builder.add(StaticStrings::ErrorMessage, VPackValue(errorMessage));
  }
  builder.add(StaticStrings::Code, VPackValue(static_cast<int>(code)));
  builder.close();

  sendSimpleResponse(code, respType, messageId, std::move(buffer));
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

// Handle a request during the server startup
void CommTask::handleRequestStartup(std::shared_ptr<RestHandler> handler) {
  // We just injected the request pointer before calling this method
  TRI_ASSERT(handler->request() != nullptr);

  RequestLane lane = handler->determineRequestLane();
  ContentType respType = handler->request()->contentTypeResponse();
  uint64_t mid = handler->messageId();

  // only fast lane handlers are allowed during startup
  TRI_ASSERT(lane == RequestLane::CLIENT_FAST);
  if (lane != RequestLane::CLIENT_FAST) {
    sendErrorResponse(ResponseCode::SERVICE_UNAVAILABLE, respType, mid,
                      TRI_ERROR_HTTP_SERVICE_UNAVAILABLE,
                      "service unavailable due to startup");
    return;
  }
  // note that in addition to the CLIENT_FAST request lane, another
  // prerequisite for serving a request during startup is that the handler
  // is registered via GeneralServerFeature::defineInitialHandlers().
  // only the handlers listed there will actually be responded to.
  // requests to any other handlers will be responded to with HTTP 503.

  handler->trackQueueStart();
  LOG_TOPIC("96766", DEBUG, Logger::REQUESTS)
      << "Handling startup request " << (void*)this << " on path "
      << handler->request()->requestPath() << " on lane " << lane;

  handler->trackQueueEnd();
  handler->trackTaskStart();

  handler->runHandler([self = shared_from_this()](rest::RestHandler* handler) {
    handler->trackTaskEnd();
    try {
      // Pass the response to the io context
      self->sendResponse(handler->stealResponse(),
                         handler->stealRequestStatistics());
    } catch (...) {
      LOG_TOPIC("e1322", WARN, Logger::REQUESTS)
          << "got an exception while sending response, closing connection";
      self->stop();
    }
  });
}

// Execute a request by queueing it in the scheduler and having it executed via
// a scheduler worker thread eventually.
void CommTask::handleRequestSync(std::shared_ptr<RestHandler> handler) {
  DTRACE_PROBE2(arangod, CommTaskHandleRequestSync, this, handler.get());

  RequestLane lane = handler->determineRequestLane();
  handler->trackQueueStart();
  // We just injected the request pointer before calling this method
  TRI_ASSERT(handler->request() != nullptr);
  LOG_TOPIC("ecd0a", DEBUG, Logger::REQUESTS)
      << "Handling request " << (void*)this << " on path "
      << handler->request()->requestPath() << " on lane " << lane;

  ContentType respType = handler->request()->contentTypeResponse();
  uint64_t mid = handler->messageId();

  // queue the operation for execution in the scheduler
  auto cb = [self = shared_from_this(),
             handler = std::move(handler)]() mutable {
    handler->trackQueueEnd();
    handler->trackTaskStart();

    handler->runHandler([self = std::move(self)](rest::RestHandler* handler) {
      handler->trackTaskEnd();
      try {
        // Pass the response to the io context
        self->sendResponse(handler->stealResponse(),
                           handler->stealRequestStatistics());
      } catch (...) {
        LOG_TOPIC("fc834", WARN, Logger::REQUESTS)
            << "got an exception while sending response, closing connection";
        self->stop();
      }
    });
  };

  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
  bool ok = SchedulerFeature::SCHEDULER->tryBoundedQueue(lane, std::move(cb));

  if (!ok) {
    sendErrorResponse(rest::ResponseCode::SERVICE_UNAVAILABLE, respType, mid,
                      TRI_ERROR_QUEUE_FULL);
  }
}

// handle a request which came in with the x-arango-async header
bool CommTask::handleRequestAsync(std::shared_ptr<RestHandler> handler,
                                  uint64_t* jobId) {
  if (_server.server().isStopping()) {
    return false;
  }

  RequestLane lane = handler->determineRequestLane();
  handler->trackQueueStart();

  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);

  if (jobId != nullptr) {
    auto& jobManager = _generalServerFeature.jobManager();
    try {
      // This will throw if a soft shutdown is already going on on a
      // coordinator. But this can also throw if we have an
      // out of memory situation, so we better handle this anyway.
      jobManager.initAsyncJob(handler);
    } catch (std::exception const& exc) {
      LOG_TOPIC("fee34", INFO, Logger::STARTUP)
          << "Async job rejected, exception: " << exc.what();
      return false;
    }
    *jobId = handler->handlerId();

    // callback will persist the response with the AsyncJobManager
    return SchedulerFeature::SCHEDULER->tryBoundedQueue(
        lane, [handler = std::move(handler), manager(&jobManager)] {
          handler->trackQueueEnd();
          handler->trackTaskStart();

          handler->runHandler([manager](RestHandler* h) {
            h->trackTaskEnd();
            manager->finishAsyncJob(h);
          });
        });
  } else {
    // here the response will just be ignored
    return SchedulerFeature::SCHEDULER->tryBoundedQueue(
        lane, [handler = std::move(handler)] {
          handler->trackQueueEnd();
          handler->trackTaskStart();

          handler->runHandler([](RestHandler* h) { h->trackTaskEnd(); });
        });
  }
}

/// @brief checks the access rights for a specified path
CommTask::Flow CommTask::canAccessPath(auth::TokenCache::Entry const& token,
                                       GeneralRequest& req) const {
  if (!_auth->isActive()) {
    // no authentication required at all
    return Flow::Continue;
  }

  std::string const& path = req.requestPath();

  auto const& ap = token.allowedPaths();
  if (!ap.empty()) {
    if (std::find(ap.begin(), ap.end(), path) == ap.end()) {
      return Flow::Abort;
    }
  }

  bool userAuthenticated = req.authenticated();
  Flow result = userAuthenticated ? Flow::Continue : Flow::Abort;

  auto vc = basics::downCast<VocbaseContext>(req.requestContext());
  TRI_ASSERT(vc != nullptr);
  // deny access to database with NONE
  if (result == Flow::Continue &&
      vc->databaseAuthLevel() == auth::Level::NONE) {
    result = Flow::Abort;
    LOG_TOPIC("0898a", TRACE, Logger::AUTHORIZATION)
        << "Access forbidden to " << path;
  }

  // we need to check for some special cases, where users may be allowed
  // to proceed even unauthorized
  if (result == Flow::Abort) {
#ifdef ARANGODB_HAVE_DOMAIN_SOCKETS
    // check if we need to run authentication for this type of
    // endpoint
    ConnectionInfo const& ci = req.connectionInfo();

    if (ci.endpointType == Endpoint::DomainType::UNIX &&
        !_auth->authenticationUnixSockets()) {
      // no authentication required for unix domain socket connections
      result = Flow::Continue;
    }
#endif

    if (result == Flow::Abort && _auth->authenticationSystemOnly()) {
      // authentication required, but only for /_api, /_admin etc.
      if (!path.empty()) {
        // check if path starts with /_
        // or path begins with /
        if (path[0] != '/' || (path.size() > 1 && path[1] != '_')) {
          // simon: upgrade rights for Foxx apps. FIXME
          result = Flow::Continue;
          vc->forceSuperuser();
          LOG_TOPIC("e2880", TRACE, Logger::AUTHORIZATION)
              << "Upgrading rights for " << path;
        }
      }
    }

    if (result == Flow::Abort) {
      std::string const& username = req.user();

      if (path == "/" || path.starts_with(::pathPrefixOpen) ||
          path.starts_with(::pathPrefixAdminAardvark) ||
          path == "/_admin/server/availability") {
        // mop: these paths are always callable...they will be able to check
        // req.user when it could be validated
        result = Flow::Continue;
        vc->forceSuperuser();
      } else if (userAuthenticated && path == "/_api/cluster/endpoints") {
        // allow authenticated users to access cluster/endpoints
        result = Flow::Continue;
        // vc->forceReadOnly();
      } else if (req.requestType() == RequestType::POST && !username.empty() &&
                 path.starts_with(std::string{::pathPrefixApiUser} + username +
                                  '/')) {
        // simon: unauthorized users should be able to call
        // `/_api/user/<name>` to check their passwords
        result = Flow::Continue;
        vc->forceReadOnly();
      } else if (userAuthenticated && path.starts_with(::pathPrefixApiUser)) {
        result = Flow::Continue;
      }
    }
  }

  return result;
}

/// deny credentialed requests or not (only CORS)
bool CommTask::allowCorsCredentials(std::string const& origin) const {
  // default is to allow nothing
  bool allowCredentials = false;
  if (origin.empty()) {
    return allowCredentials;
  }  // else handle origin headers

  // if the request asks to allow credentials, we'll check against the
  // configured allowed list of origins
  std::vector<std::string> const& accessControlAllowOrigins =
      _generalServerFeature.accessControlAllowOrigins();

  if (!accessControlAllowOrigins.empty()) {
    if (accessControlAllowOrigins[0] == "*") {
      // special case: allow everything
      allowCredentials = true;
    } else if (!origin.empty()) {
      // copy origin string
      if (origin[origin.size() - 1] == '/') {
        // strip trailing slash
        auto result = std::find(accessControlAllowOrigins.begin(),
                                accessControlAllowOrigins.end(),
                                origin.substr(0, origin.size() - 1));
        allowCredentials = (result != accessControlAllowOrigins.end());
      } else {
        auto result = std::find(accessControlAllowOrigins.begin(),
                                accessControlAllowOrigins.end(), origin);
        allowCredentials = (result != accessControlAllowOrigins.end());
      }
    } else {
      TRI_ASSERT(!allowCredentials);
    }
  }
  return allowCredentials;
}

/// handle an OPTIONS request
void CommTask::processCorsOptions(std::unique_ptr<GeneralRequest> req,
                                  std::string const& origin) {
  auto resp = createResponse(rest::ResponseCode::OK, req->messageId());
  resp->setHeaderNCIfNotSet(StaticStrings::Allow, StaticStrings::CorsMethods);

  if (!origin.empty()) {
    LOG_TOPIC("e1cfa", DEBUG, arangodb::Logger::REQUESTS)
        << "got CORS preflight request";
    std::string const allowHeaders = StringUtils::trim(
        req->header(StaticStrings::AccessControlRequestHeaders));

    // send back which HTTP methods are allowed for the resource
    // we'll allow all
    resp->setHeaderNCIfNotSet(StaticStrings::AccessControlAllowMethods,
                              StaticStrings::CorsMethods);

    if (!allowHeaders.empty()) {
      // allow all extra headers the client requested
      // we don't verify them here. the worst that can happen is that the
      // client sends some broken headers and then later cannot access the data
      // on the server. that's a client problem.
      resp->setHeaderNCIfNotSet(StaticStrings::AccessControlAllowHeaders,
                                allowHeaders);

      LOG_TOPIC("55413", TRACE, arangodb::Logger::REQUESTS)
          << "client requested validation of the following headers: "
          << allowHeaders;
    }

    // set caching time (hard-coded value)
    resp->setHeaderNCIfNotSet(StaticStrings::AccessControlMaxAge,
                              StaticStrings::N1800);
  }

  // discard request and send response
  sendResponse(std::move(resp), stealRequestStatistics(req->messageId()));
}

auth::TokenCache::Entry CommTask::checkAuthHeader(GeneralRequest& req,
                                                  ServerState::Mode mode) {
  bool found;
  std::string const& authStr = req.header(StaticStrings::Authorization, found);
  if (!found) {
    if (this->_auth->isActive()) {
      events::CredentialsMissing(req);
      return auth::TokenCache::Entry::Unauthenticated();
    }
    events::Authenticated(req, AuthenticationMethod::NONE);
    return auth::TokenCache::Entry::Superuser();
  }

  std::string::size_type methodPos = authStr.find(' ');
  if (methodPos == std::string::npos) {
    events::UnknownAuthenticationMethod(req);
    return auth::TokenCache::Entry::Unauthenticated();
  }

  // skip over authentication method and following whitespace
  char const* auth = authStr.c_str() + methodPos;
  while (*auth == ' ') {
    ++auth;
  }

  LOG_TOPIC_IF("c4536", DEBUG, arangodb::Logger::REQUESTS,
               Logger::logRequestParameters())
      << "\"authorization-header\",\"" << (void*)this
      << "\",SENSITIVE_DETAILS_HIDDEN";

  AuthenticationMethod authMethod = AuthenticationMethod::NONE;
  if (authStr.size() >= 6) {
    if (strncasecmp(authStr.c_str(), "basic ", 6) == 0) {
      authMethod = AuthenticationMethod::BASIC;
    } else if (strncasecmp(authStr.c_str(), "bearer ", 7) == 0) {
      authMethod = AuthenticationMethod::JWT;
    }
  }

  req.setAuthenticationMethod(authMethod);
  if (authMethod == AuthenticationMethod::NONE) {
    events::UnknownAuthenticationMethod(req);
    return auth::TokenCache::Entry::Unauthenticated();
  }

  if (authMethod != AuthenticationMethod::JWT &&
      mode == ServerState::Mode::STARTUP) {
    // during startup, the normal authentication is not available
    // yet. so we have to refuse all requests that do not use
    // a JWT.
    // do not audit-log anything in this case, because there is
    // nothing we can do to verify the credentials.
    return auth::TokenCache::Entry::Unauthenticated();
  }

  auto authToken =
      this->_auth->tokenCache().checkAuthentication(authMethod, mode, auth);
  req.setAuthenticated(authToken.authenticated());
  req.setTokenExpiry(authToken.expiry());
  req.setUser(authToken.username());  // do copy here, so that we do not
  // invalidate the member
  if (authToken.authenticated()) {
    events::Authenticated(req, authMethod);
  } else {
    events::CredentialsBad(req, authMethod);
  }
  return authToken;
}

/// decompress content
Result CommTask::handleContentEncoding(GeneralRequest& req) {
  // TODO consider doing the decoding on the fly
  auto decode = [&](std::string const& header,
                    std::string const& encoding) -> Result {
    if (this->_auth->isActive() && !req.authenticated() &&
        !_generalServerFeature
             .handleContentEncodingForUnauthenticatedRequests()) {
      return {TRI_ERROR_FORBIDDEN,
              "support for handling Content-Encoding headers is turned off for "
              "unauthenticated requests"};
    }

    std::string_view raw = req.rawPayload();
    uint8_t* src = reinterpret_cast<uint8_t*>(const_cast<char*>(raw.data()));
    size_t len = raw.size();

    if (encoding == StaticStrings::EncodingGzip) {
      VPackBuffer<uint8_t> dst;
      if (ErrorCode r = arangodb::encoding::gzipUncompress(src, len, dst);
          r != TRI_ERROR_NO_ERROR) {
        return {
            r,
            "a decoding error occurred while handling Content-Encoding: gzip"};
      }
      req.setPayload(std::move(dst));
      // as we have decoded, remove the encoding header.
      // this prevents duplicate decoding
      req.removeHeader(header);
      return {};
    } else if (encoding == StaticStrings::EncodingDeflate) {
      VPackBuffer<uint8_t> dst;
      if (ErrorCode r = arangodb::encoding::zlibInflate(src, len, dst);
          r != TRI_ERROR_NO_ERROR) {
        return {r,
                "a decoding error occurred while handling Content-Encoding: "
                "deflate"};
      }
      req.setPayload(std::move(dst));
      // as we have decoded, remove the encoding header.
      // this prevents duplicate decoding
      req.removeHeader(header);
      return {};
    } else if (encoding == StaticStrings::EncodingArangoLz4) {
      VPackBuffer<uint8_t> dst;
      if (ErrorCode r = arangodb::encoding::lz4Uncompress(src, len, dst);
          r != TRI_ERROR_NO_ERROR) {
        return {
            r,
            "a decoding error occurred while handling Content-Encoding: lz4"};
      }
      req.setPayload(std::move(dst));
      // as we have decoded, remove the encoding header.
      // this prevents duplicate decoding
      req.removeHeader(header);
      return {};
    }
    // unknown encoding. let it through without modifying the request body.
    return {};
  };

  bool found;
  if (std::string const& val =
          req.header(StaticStrings::TransferEncoding, found);
      found) {
    return decode(StaticStrings::TransferEncoding, val);
  }

  if (std::string const& val =
          req.header(StaticStrings::ContentEncoding, found);
      found) {
    return decode(StaticStrings::ContentEncoding, val);
  }
  return {};
}
