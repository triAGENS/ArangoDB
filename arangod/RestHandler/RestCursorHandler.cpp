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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "RestCursorHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Basics/Exceptions.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Transaction/OperationOrigin.h"
#include "Utils/Cursor.h"
#include "Utils/CursorRepository.h"
#include "Utils/Events.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/Value.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestCursorHandler::RestCursorHandler(
    ArangodServer& server, GeneralRequest* request, GeneralResponse* response,
    arangodb::aql::QueryRegistry* queryRegistry)
    : RestVocbaseBaseHandler(server, request, response),
      _queryKilled(false),
      _queryRegistry(queryRegistry),
      _cursor(nullptr) {}

RestCursorHandler::~RestCursorHandler() { releaseCursor(); }

RequestLane RestCursorHandler::lane() const {
  if (_request->requestType() != rest::RequestType::POST ||
      !_request->suffixes().empty()) {
    // continuing an existing query or cleaning up its resources
    // gets higher priority than starting new queries
    return RequestLane::CONTINUATION;
  }

  // low priority for starting new queries
  return RequestLane::CLIENT_AQL;
}

RestStatus RestCursorHandler::execute() {
  // extract the sub-request type
  rest::RequestType const type = _request->requestType();

  if (type == rest::RequestType::POST) {
    if (_request->suffixes().size() == 0) {
      // POST /_api/cursor
      return createQueryCursor();
    } else if (_request->suffixes().size() == 1) {
      // POST /_api/cursor/cursor-id
      return modifyQueryCursor();
    }
    // POST /_api/cursor/cursor-id/batch-id
    return showLatestBatch();
  } else if (type == rest::RequestType::PUT) {
    return modifyQueryCursor();
  } else if (type == rest::RequestType::DELETE_REQ) {
    return deleteQueryCursor();
  }
  generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  return RestStatus::DONE;
}

RestStatus RestCursorHandler::continueExecute() {
  if (wasCanceled()) {
    generateError(rest::ResponseCode::GONE, TRI_ERROR_QUERY_KILLED);
    return RestStatus::DONE;
  }

  if (!_response->isResponseEmpty()) {
    // an exception occurred in one of the suspension points
    return RestStatus::DONE;
  }

  // extract the sub-request type
  rest::RequestType const type = _request->requestType();

  if (_query != nullptr) {  // non-stream query
    if (type == rest::RequestType::POST || type == rest::RequestType::PUT) {
      return processQuery();
    }
  } else if (_cursor) {  // stream cursor query
    if (type == rest::RequestType::POST) {
      if (_request->suffixes().size() == 0) {
        // POST /_api/cursor
        return generateCursorResult(rest::ResponseCode::CREATED);
      }
      // POST /_api/cursor/cursor-id
      return generateCursorResult(ResponseCode::OK);
    } else if (type == rest::RequestType::PUT) {
      if (_request->requestPath() == SIMPLE_QUERY_ALL_PATH) {
        // RestSimpleQueryHandler::allDocuments uses PUT for cursor creation
        return generateCursorResult(ResponseCode::CREATED);
      }
      return generateCursorResult(ResponseCode::OK);
    }
  }

  // Other parts of the query cannot be paused
  TRI_ASSERT(false) << requestToString(type) << " " << _request->fullUrl()
                    << " " << _request->parameters();
  return RestStatus::DONE;
}

void RestCursorHandler::shutdownExecute(bool isFinalized) noexcept {
  auto sg = arangodb::scopeGuard(
      [&]() noexcept { RestVocbaseBaseHandler::shutdownExecute(isFinalized); });

  // request not done yet
  if (!isFinalized) {
    return;
  }

  releaseCursor();

  // only trace create cursor requests
  if (_request->requestType() != rest::RequestType::POST ||
      _request->suffixes().size() > 0) {
    return;
  }

  // destroy the query context.
  // this is needed because the context is managing resources (e.g. leases
  // for a managed transaction) that we want to free as early as possible
  _queryResult.context.reset();
}

void RestCursorHandler::cancel() {
  RestVocbaseBaseHandler::cancel();
  return cancelQuery();
}

/// @brief register the query either as streaming cursor or in _query
/// the query is not executed here.
/// this method is also used by derived classes
///
/// return If true, we need to continue processing,
///        If false we are done (error or stream)
futures::Future<RestStatus> RestCursorHandler::registerQueryOrCursor(
    velocypack::Slice slice, transaction::OperationOrigin operationOrigin) {
  TRI_ASSERT(_query == nullptr);

  if (!slice.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_QUERY_EMPTY);
    co_return RestStatus::DONE;
  }
  VPackSlice querySlice = slice.get("query");
  if (!querySlice.isString() || querySlice.getStringLength() == 0) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_QUERY_EMPTY);
    co_return RestStatus::DONE;
  }

  VPackSlice bindVars = slice.get("bindVars");
  if (!bindVars.isNone()) {
    if (!bindVars.isObject() && !bindVars.isNull()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_TYPE_ERROR,
                    "expecting object for <bindVars>");
      co_return RestStatus::DONE;
    }
  }

  std::shared_ptr<VPackBuilder> bindVarsBuilder;
  if (!bindVars.isNone()) {
    bindVarsBuilder = std::make_shared<VPackBuilder>(bindVars);
  }

  TRI_ASSERT(_options == nullptr);
  buildOptions(slice);
  TRI_ASSERT(_options != nullptr);
  VPackSlice opts = _options->slice();

  bool stream = VelocyPackHelper::getBooleanValue(opts, "stream", false);
  if (ServerState::instance()->isDBServer()) {
    stream = false;
  }
  size_t batchSize =
      VelocyPackHelper::getNumericValue<size_t>(opts, "batchSize", 1000);
  double ttl = VelocyPackHelper::getNumericValue<double>(
      opts, "ttl", _queryRegistry->defaultTTL());
  bool count = VelocyPackHelper::getBooleanValue(opts, "count", false);
  bool retriable = VelocyPackHelper::getBooleanValue(opts, "allowRetry", false);

  // simon: access mode can always be write on the coordinator
  AccessMode::Type mode = AccessMode::Type::WRITE;

  auto query = aql::Query::create(
      co_await createTransactionContext(mode, operationOrigin),
      aql::QueryString(querySlice.stringView()), std::move(bindVarsBuilder),
      aql::QueryOptions(opts));

  if (stream) {
    TRI_ASSERT(!ServerState::instance()->isDBServer());
    if (count) {
      generateError(Result(TRI_ERROR_BAD_PARAMETER,
                           "cannot use 'count' option for a streaming query"));
      co_return RestStatus::DONE;
    }

    CursorRepository* cursors = _vocbase.cursorRepository();
    TRI_ASSERT(cursors != nullptr);
    auto aborter = std::make_shared<aql::QueryAborter>(query);
    _cursor = cursors->createQueryStream(std::move(query), batchSize, ttl,
                                         retriable, operationOrigin, aborter);
    // Throws if soft shutdown is ongoing!
    _cursor->setWakeupHandler(withLogContext(
        [self = shared_from_this()]() { return self->wakeupHandler(); }));

    co_return generateCursorResult(rest::ResponseCode::CREATED);
  }

  // non-stream case. Execute query, then build a cursor
  // with the entire result set.
  if (!ServerState::instance()->isDBServer()) {
    auto ss = query->sharedState();
    TRI_ASSERT(ss != nullptr);
    if (ss == nullptr) {
      generateError(Result(TRI_ERROR_INTERNAL, "invalid query state"));
      co_return RestStatus::DONE;
    }

    ss->setWakeupHandler(withLogContext(
        [self = shared_from_this()] { return self->wakeupHandler(); }));
  }

  registerQuery(std::move(query));
  co_return processQuery();
}

/// @brief Process the query registered in _query.
/// The function is repeatable, so whenever we need to WAIT
/// in AQL we can post a handler calling this function again.
RestStatus RestCursorHandler::processQuery() {
  auto query = [this]() {
    std::unique_lock<std::mutex> mutexLocker{_queryLock};

    if (_query == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          "Illegal state in RestCursorHandler, query not found.");
    }
    return _query;
  }();

  {
    // always clean up
    auto guard = scopeGuard([this]() noexcept { unregisterQuery(); });

    auto aborter = std::make_shared<aql::QueryAborter>(_query);
    // continue handler is registered earlier
    auto state = _query->execute(aborter, _queryResult);

    if (state == aql::ExecutionState::WAITING) {
      guard.cancel();
      return RestStatus::WAITING;
    }
    TRI_ASSERT(state == aql::ExecutionState::DONE);
  }

  // We cannot get into HASMORE here, or we would lose results.
  return handleQueryResult();
}

// non stream case, result is complete
RestStatus RestCursorHandler::handleQueryResult() {
  TRI_ASSERT(_query == nullptr);
  if (_queryResult.result.fail()) {
    if (_queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (_queryResult.result.is(TRI_ERROR_QUERY_KILLED) && wasCanceled())) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REQUEST_CANCELED);
    }

    THROW_ARANGO_EXCEPTION(_queryResult.result);
  }

  VPackSlice qResult = _queryResult.data->slice();

  if (qResult.isNone()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  TRI_ASSERT(qResult.isArray());
  TRI_ASSERT(_options != nullptr);
  VPackSlice opts = _options->slice();

  size_t batchSize =
      VelocyPackHelper::getNumericValue<size_t>(opts, "batchSize", 1000);
  double ttl = VelocyPackHelper::getNumericValue<double>(opts, "ttl", 30);
  bool count = VelocyPackHelper::getBooleanValue(opts, "count", false);
  bool retriable = VelocyPackHelper::getBooleanValue(opts, "allowRetry", false);

  _response->setContentType(rest::ContentType::JSON);
  size_t n = static_cast<size_t>(qResult.length());
  if (n <= batchSize) {
    // result is smaller than batchSize and will be returned directly. no need
    // to create a cursor

    if (_queryResult.allowDirtyReads) {
      setOutgoingDirtyReadsHeader(true);
    }

    VPackOptions options = VPackOptions::Defaults;
    options.buildUnindexedArrays = true;
    options.buildUnindexedObjects = true;

    // conservatively allocate a few bytes per value to be returned
    auto res = TRI_ERROR_NO_ERROR;
    if (n >= 10000) {
      res = _response->reservePayload(128 * 1024);
    } else if (n >= 1000) {
      res = _response->reservePayload(64 * 1024);
    } else {
      res = _response->reservePayload(n * 48);
    }

    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }

    VPackBuffer<uint8_t> buffer;
    VPackBuilder result(buffer, &options);
    try {
      VPackObjectBuilder b(&result);
      result.add(VPackValue("result"));
      result.add(VPackValue(qResult.begin(), VPackValueType::External));
      result.add("hasMore", VPackValue(false));
      if (VelocyPackHelper::getBooleanValue(opts, "count", false)) {
        result.add("count", VPackValue(n));
      }
      result.add("cached", VPackValue(_queryResult.cached));
      if (!_queryResult.extra) {
        result.add("extra", VPackValue(VPackValueType::Object));
        // no warnings
        result.add("warnings", VPackSlice::emptyArraySlice());
        result.close();
      } else {
        result.add("extra", _queryResult.extra->slice());
      }
      result.add(StaticStrings::Error, VPackValue(false));
      result.add(StaticStrings::Code,
                 VPackValue(static_cast<int>(ResponseCode::CREATED)));
    } catch (std::exception const& ex) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
    }
    generateResult(rest::ResponseCode::CREATED, std::move(buffer),
                   _queryResult.context);
    // directly after returning from here, we will free the query's context and
    // free the resources it uses (e.g. leases for a managed transaction). this
    // way the server can send back the query result to the client and the
    // client can make follow-up requests on the same transaction (e.g.
    // trx.commit()) without the server code for freeing the resources and the
    // client code racing for who's first

    return RestStatus::DONE;
  } else {
    // result is bigger than batchSize, and a cursor will be created
    CursorRepository* cursors = _vocbase.cursorRepository();
    TRI_ASSERT(cursors != nullptr);
    TRI_ASSERT(_queryResult.data.get() != nullptr);
    // steal the query result, cursor will take over the ownership
    _cursor = cursors->createFromQueryResult(std::move(_queryResult), batchSize,
                                             ttl, count, retriable);
    // throws if a coordinator soft shutdown is ongoing

    return generateCursorResult(rest::ResponseCode::CREATED);
  }
}

/// @brief returns the short id of the server which should handle this request
ResultT<std::pair<std::string, bool>> RestCursorHandler::forwardingTarget() {
  auto base = RestVocbaseBaseHandler::forwardingTarget();
  if (base.ok() && !std::get<0>(base.get()).empty()) {
    return base;
  }

  rest::RequestType type = _request->requestType();
  if (type != rest::RequestType::POST && type != rest::RequestType::PUT &&
      type != rest::RequestType::DELETE_REQ) {
    // request forwarding only exists for
    // POST /_api/cursor/cursor-id
    // PUT /_api/cursor/cursor-id
    // DELETE /_api/cursor/cursor-id
    return {std::make_pair(StaticStrings::Empty, false)};
  }

  std::vector<std::string> const& suffixes = _request->suffixes();
  if (suffixes.empty()) {
    return {std::make_pair(StaticStrings::Empty, false)};
  }

  uint64_t tick = arangodb::basics::StringUtils::uint64(suffixes[0]);
  uint32_t sourceServer = TRI_ExtractServerIdFromTick(tick);

  if (sourceServer == ServerState::instance()->getShortId()) {
    return {std::make_pair(StaticStrings::Empty, false)};
  }
  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
  auto coordinatorId = ci.getCoordinatorByShortID(sourceServer);

  if (coordinatorId.empty()) {
    return ResultT<std::pair<std::string, bool>>::error(
        TRI_ERROR_CURSOR_NOT_FOUND, "cannot find target server for cursor id");
  }

  return {std::make_pair(std::move(coordinatorId), false)};
}

/// @brief register the currently running query
void RestCursorHandler::registerQuery(
    std::shared_ptr<arangodb::aql::Query> query) {
  std::lock_guard mutexLocker{_queryLock};

  if (_queryKilled) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_REQUEST_CANCELED);
  }

  TRI_ASSERT(_query == nullptr);
  TRI_ASSERT(query != nullptr);
  _query = std::move(query);
}

/// @brief unregister the currently running query
void RestCursorHandler::unregisterQuery() noexcept {
  TRI_IF_FAILURE(
      "RestCursorHandler::directKillBeforeQueryResultIsGettingHandled") {
    _query->debugKillQuery();
  }
  std::lock_guard mutexLocker{_queryLock};
  _query.reset();
}

/// @brief cancel the currently running query
void RestCursorHandler::cancelQuery() {
  std::lock_guard mutexLocker{_queryLock};

  if (_query != nullptr) {
    // cursor is canceled. now remove the continue handler we may have
    // registered in the query
    if (_query->sharedState()) {
      _query->sharedState()->resetWakeupHandler();
    }

    _query->setKillFlag();
  }
  _queryKilled = true;
}

bool RestCursorHandler::wasCanceled() const {
  std::lock_guard mutexLocker{_queryLock};
  return _queryKilled;
}

/// @brief build options for the query as JSON
void RestCursorHandler::buildOptions(velocypack::Slice slice) {
  _options = std::make_shared<VPackBuilder>();
  VPackObjectBuilder obj(_options.get());

  bool hasCache = false;
  bool hasMemoryLimit = false;
  VPackSlice opts = slice.get("options");
  // "stream" option is important, so also accept it on top level and not only
  // inside options
  bool isStream = VelocyPackHelper::getBooleanValue(slice, "stream", false);

  // Look for allowDirtyReads header:
  bool allowDirtyReads = false;
  bool found = false;
  bool sawDirtyReads = false;
  std::string const& val =
      _request->header(StaticStrings::AllowDirtyReads, found);
  if (found && StringUtils::boolean(val)) {
    allowDirtyReads = true;
  }

  if (opts.isObject()) {
    if (!isStream) {
      isStream = VelocyPackHelper::getBooleanValue(opts, "stream", false);
    }
    for (auto it : VPackObjectIterator(opts)) {
      if (!it.key.isString() || it.value.isNone()) {
        continue;
      }
      std::string_view keyName = it.key.stringView();
      if (keyName == "allowDirtyReads") {
        // We will set the flag in the options, if either the HTTP header
        // is set, or we see true here in the slice:
        sawDirtyReads = true;
        allowDirtyReads |= it.value.isTrue();
        _options->add(keyName, VPackValue(allowDirtyReads));
      }
      if (keyName == "count" || keyName == "batchSize" || keyName == "ttl" ||
          keyName == "stream" || (isStream && keyName == "fullCount")) {
        continue;  // filter out top-level keys
      } else if (keyName == "cache") {
        hasCache = true;  // don't honor if appears below
      } else if (keyName == "memoryLimit" && it.value.isNumber()) {
        hasMemoryLimit = true;
      }
      _options->add(keyName, it.value);
    }
  }
  if (!sawDirtyReads && allowDirtyReads) {
    _options->add("allowDirtyReads", VPackValue(true));
  }

  if (ServerState::instance()->isDBServer()) {
    // we do not support creating streaming cursors on DB-Servers, at all.
    // always turn such cursors into non-streaming cursors.
    isStream = false;
  }

  _options->add("stream", VPackValue(isStream));

  if (!isStream) {  // ignore cache & count for streaming queries
    bool val = VelocyPackHelper::getBooleanValue(slice, "count", false);
    _options->add("count", VPackValue(val));
    if (!hasCache && slice.hasKey("cache")) {
      val = VelocyPackHelper::getBooleanValue(slice, "cache", false);
      _options->add("cache", VPackValue(val));
    }
  }

  VPackSlice batchSize = slice.get("batchSize");
  if (batchSize.isNumber()) {
    if ((batchSize.isDouble() && batchSize.getDouble() == 0.0) ||
        (batchSize.isInteger() && batchSize.getNumericValue<int64_t>() <= 0)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_TYPE_ERROR, "expecting positive value for <batchSize>");
    }
    _options->add("batchSize", batchSize);
  } else {
    _options->add("batchSize", VPackValue(1000));
  }

  if (!hasMemoryLimit) {
    VPackSlice memoryLimit = slice.get("memoryLimit");
    if (memoryLimit.isNumber()) {
      _options->add("memoryLimit", memoryLimit);
    }
  }

  VPackSlice ttl = slice.get("ttl");
  _options->add("ttl", VPackValue(ttl.isNumber() && ttl.getNumber<double>() > 0
                                      ? ttl.getNumber<double>()
                                      : _queryRegistry->defaultTTL()));
}

/// @brief append the contents of the cursor into the response body
/// this function will also take care of the cursor and return it to the
/// registry if required
RestStatus RestCursorHandler::generateCursorResult(rest::ResponseCode code) {
  TRI_ASSERT(_cursor != nullptr);

  // dump might delete the cursor
  std::shared_ptr<transaction::Context> ctx = _cursor->context();

  VPackBuilder builder;
  builder.openObject(/*unindexed*/ true);

  auto const [state, r] = _cursor->dump(builder);

  if (state == aql::ExecutionState::WAITING) {
    builder.clear();
    TRI_ASSERT(r.ok());
    return RestStatus::WAITING;
  }

  if (_cursor->allowDirtyReads()) {
    setOutgoingDirtyReadsHeader(true);
  }

  if (r.ok()) {
    builder.add(StaticStrings::Error, VPackValue(false));
    builder.add(StaticStrings::Code, VPackValue(static_cast<int>(code)));
    builder.close();
    _response->setContentType(rest::ContentType::JSON);
    TRI_IF_FAILURE("MakeConnectionErrorForRetry") {
      if (_cursor->isRetriable()) {
        _cursor->setLastQueryBatchObject(builder.steal());
      }
      return RestStatus::FAIL;
    }

    generateResult(code, builder.slice(), std::move(ctx));
    if (_cursor->isRetriable()) {
      _cursor->setLastQueryBatchObject(builder.steal());
    }
  } else {
    if (_cursor->isRetriable()) {
      builder.add(StaticStrings::Code,
                  VPackValue(static_cast<int>(
                      GeneralResponse::responseCode(r.errorNumber()))));
      builder.add(StaticStrings::Error, VPackValue(true));
      builder.add(StaticStrings::ErrorMessage, VPackValue(r.errorMessage()));
      builder.add(StaticStrings::ErrorNum, VPackValue(r.errorNumber()));
      builder.close();
      _cursor->setLastQueryBatchObject(builder.steal());
    }
    // builder can be in a broken state here. simply return the error
    generateError(r);
  }

  return RestStatus::DONE;
}

RestStatus RestCursorHandler::createQueryCursor() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (!suffixes.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting POST /_api/cursor");
    return RestStatus::DONE;
  }

  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);

  if (!parseSuccess) {
    // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  if (body.isEmptyObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_CORRUPTED_JSON);
    return RestStatus::DONE;
  }

  TRI_ASSERT(_query == nullptr);
  return waitForFuture(registerQueryOrCursor(
      body, transaction::OperationOriginAQL{"running AQL query"}));
}

/// @brief shows the batch given by <batch-id> if it's the last cached batch
/// response on a retry, and does't advance cursor
RestStatus RestCursorHandler::showLatestBatch() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting POST /_api/cursor/<cursor-id>/<batch-id>");
    return RestStatus::DONE;
  }

  uint64_t batchId = basics::StringUtils::uint64(suffixes[1]);

  lookupCursor(suffixes[0], batchId);

  if (_cursor == nullptr) {
    // error response already built here
    return RestStatus::DONE;
  }

  _cursor->setWakeupHandler(withLogContext(
      [self = shared_from_this()]() { return self->wakeupHandler(); }));

  // POST /_api/cursor/<cid>/x and the current batchId on the server is y, then:
  //   if x == y, resend the current batch
  //   if x == y + 1, advance the cursor and return the new batch
  //   otherwise return error
  if (_cursor->isNextBatchId(batchId)) {
    return generateCursorResult(rest::ResponseCode::OK);
  }

  if (!_cursor->isCurrentBatchId(batchId)) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batch id not found");
    return RestStatus::DONE;
  }

  auto buffer = _cursor->getLastBatch();
  TRI_ASSERT(buffer != nullptr);

  _response->setContentType(rest::ContentType::JSON);
  generateResult(rest::ResponseCode::OK, VPackSlice(buffer->data()),
                 _cursor->context());

  return RestStatus::DONE;
}

RestStatus RestCursorHandler::modifyQueryCursor() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting POST /_api/cursor/<cursor-id>");
    return RestStatus::DONE;
  }

  // the call to lookupCursor will populate _cursor if the cursor can be
  // found. otherwise, _cursor will remain a nullptr and an error will
  // be written to the response
  lookupCursor(suffixes[0]);

  if (_cursor == nullptr) {
    return RestStatus::DONE;
  }

  _cursor->setWakeupHandler(withLogContext(
      [self = shared_from_this()]() { return self->wakeupHandler(); }));

  return generateCursorResult(rest::ResponseCode::OK);
}

RestStatus RestCursorHandler::deleteQueryCursor() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting DELETE /_api/cursor/<cursor-id>");
    return RestStatus::DONE;
  }

  std::string const& id = suffixes[0];

  auto cursors = _vocbase.cursorRepository();
  TRI_ASSERT(cursors != nullptr);

  auto cursorId =
      static_cast<arangodb::CursorId>(basics::StringUtils::uint64(id));
  bool found = cursors->remove(cursorId);

  if (!found) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND);
    return RestStatus::DONE;
  }

  VPackBuilder builder;
  builder.openObject();
  builder.add("id", VPackValue(id));
  builder.add(StaticStrings::Error, VPackValue(false));
  builder.add(StaticStrings::Code,
              VPackValue(static_cast<int>(rest::ResponseCode::ACCEPTED)));
  builder.close();

  generateResult(rest::ResponseCode::ACCEPTED, builder.slice());
  return RestStatus::DONE;
}

/// @brief look up cursor by id. side-effect: populates _cursor in case cursor
/// was found. in case cursor was not found, writes an error into the response
void RestCursorHandler::lookupCursor(std::string_view id,
                                     std::optional<uint64_t> batchId) {
  TRI_ASSERT(_cursor == nullptr);

  auto cursors = _vocbase.cursorRepository();
  TRI_ASSERT(cursors != nullptr);

  auto cursorId =
      static_cast<arangodb::CursorId>(basics::StringUtils::uint64(id));
  bool busy;
  _cursor = cursors->find(cursorId, busy);

  if (_cursor == nullptr) {
    if (busy) {
      generateError(GeneralResponse::responseCode(TRI_ERROR_CURSOR_BUSY),
                    TRI_ERROR_CURSOR_BUSY);
    } else {
      generateError(GeneralResponse::responseCode(TRI_ERROR_CURSOR_NOT_FOUND),
                    TRI_ERROR_CURSOR_NOT_FOUND);
    }
    return;
  }

  TRI_ASSERT(_cursor != nullptr);

  if (batchId.has_value() && _cursor->isCurrentBatchId(batchId.value()) &&
      !_cursor->isRetriable()) {
    releaseCursor();
    TRI_ASSERT(_cursor == nullptr);

    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting allowRetry option to be true");
  }
}

void RestCursorHandler::releaseCursor() {
  if (_cursor) {
    _cursor->resetWakeupHandler();

    auto cursors = _vocbase.cursorRepository();
    TRI_ASSERT(cursors != nullptr);
    cursors->release(_cursor);
    _cursor = nullptr;
  }
}
