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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "RestAqlHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlExecuteResult.h"
#include "Aql/BlocksWithClients.h"
#include "Aql/ClusterQuery.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ProfileLevel.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/LeaseManager/LeaseId.h"
#include "Cluster/LeaseManager/LeaseManagerFeature.h"
#include "Cluster/RebootTracker.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "GeneralServer/RestHandler.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LogStructuredParamsAllowList.h"
#include "Random/RandomGenerator.h"
#include "Rest/GeneralRequest.h"
#include "Transaction/Context.h"

#include <absl/strings/str_cat.h>

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::rest;
using namespace arangodb::aql;

RestAqlHandler::RestAqlHandler(ArangodServer& server, GeneralRequest* request,
                               GeneralResponse* response, QueryRegistry* qr)
    : RestVocbaseBaseHandler(server, request, response),
      _queryRegistry(qr),
      _engine(nullptr) {
  TRI_ASSERT(_queryRegistry != nullptr);
}

RestAqlHandler::~RestAqlHandler() {
  if (_logContextQueryIdEntry) {
    LogContext::Current::popEntry(_logContextQueryIdEntry);
  }
}

// POST method for /_api/aql/setup (internal)
// Only available on DBServers in the Cluster.
// This route sets-up all the query engines required
// for a complete query on this server.
// Furthermore it directly locks all shards for this query.
// So after this route the query is ready to go.
// NOTE: As this Route LOCKS the collections, the caller
// is responsible to destroy those engines in a timely
// manner, if the engines are not called for a period
// of time, they will be garbage-collected and unlocked.
// The body is a VelocyPack with the following layout:
//  {
//    lockInfo: {
//      NONE: [<collections to not-lock],
//      READ: [<collections to read-lock],
//      WRITE: [<collections to write-lock],
//      EXCLUSIVE: [<collections with exclusive-lock]
//    },
//    options: { < query options > },
//    snippets: {
//      <queryId: {nodes: [ <nodes>]}>
//    },
//    traverserEngines: [ <infos for traverser engines> ],
//    variables: [ <variables> ]
//  }
futures::Future<futures::Unit> RestAqlHandler::setupClusterQuery() {
  // We should not intentionally call this method
  // on the wrong server. So fail during maintanence.
  // On user setup reply gracefully.
  TRI_ASSERT(ServerState::instance()->isDBServer());
  if (ADB_UNLIKELY(!ServerState::instance()->isDBServer())) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
    co_return;
  }

  TRI_IF_FAILURE("Query::setupTimeout") {
    // intentionally delay the request
    std::this_thread::sleep_for(
        std::chrono::milliseconds(RandomGenerator::interval(uint32_t(2000))));
  }

  TRI_IF_FAILURE("Query::setupTimeoutFailSequence") {
    // simulate lock timeout during query setup
    uint32_t r = 100;
    TRI_IF_FAILURE("Query::setupTimeoutFailSequenceRandom") {
      r = RandomGenerator::interval(uint32_t(100));
    }
    if (r >= 96) {
      std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    }
  }

  bool fastPath = false;  // Default false, now check HTTP header:
  if (!_request->header(StaticStrings::AqlFastPath).empty()) {
    fastPath = true;
  }

  bool success = false;
  VPackSlice querySlice = this->parseVPackBody(success);
  if (!success) {
    // if no success here, generateError will have been called already
    LOG_TOPIC("ef4ca", ERR, arangodb::Logger::AQL)
        << "Failed to setup query. Could not "
           "parse the transmitted plan. "
           "Aborting query.";
    co_return;
  }

  QueryId clusterQueryId = 0;
  // this is an optional attribute that 3.8 coordinators will send, but
  // older versions won't send.
  // if set, it is the query id that will be used for this particular query
  if (auto queryIdSlice = querySlice.get("clusterQueryId");
      queryIdSlice.isNumber()) {
    clusterQueryId = queryIdSlice.getNumber<QueryId>();
    TRI_ASSERT(clusterQueryId > 0);
  }

  TRI_ASSERT(_logContextQueryIdValue == nullptr);
  _logContextQueryIdValue = LogContext::makeValue()
                                .with<structuredParams::QueryId>(clusterQueryId)
                                .share();
  TRI_ASSERT(_logContextQueryIdEntry == nullptr);
  _logContextQueryIdEntry =
      LogContext::Current::pushValues(_logContextQueryIdValue);

  VPackSlice lockInfoSlice = querySlice.get("lockInfo");

  if (!lockInfoSlice.isObject()) {
    LOG_TOPIC("19e7e", ERR, arangodb::Logger::AQL)
        << "Invalid VelocyPack: \"lockInfo\" is required but not an object.";
    generateError(rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
                  "body must be an object with attribute \"lockInfo\"");
    co_return;
  }

  VPackSlice optionsSlice = querySlice.get("options");
  if (!optionsSlice.isObject()) {
    LOG_TOPIC("1a8a1", ERR, arangodb::Logger::AQL)
        << "Invalid VelocyPack: \"options\" attribute missing.";
    generateError(rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
                  "body must be an object with attribute \"options\"");
    co_return;
  }

  VPackSlice snippetsSlice = querySlice.get("snippets");
  if (!snippetsSlice.isObject()) {
    LOG_TOPIC("5bd07", ERR, arangodb::Logger::AQL)
        << "Invalid VelocyPack: \"snippets\" attribute missing.";
    generateError(rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
                  "body must be an object with attribute \"snippets\"");
    co_return;
  }

  VPackSlice traverserSlice = querySlice.get("traverserEngines");
  if (!traverserSlice.isNone() && !traverserSlice.isArray()) {
    LOG_TOPIC("69f64", ERR, arangodb::Logger::AQL)
        << "Invalid VelocyPack: \"traverserEngines\" attribute is not an "
           "array.";
    generateError(
        rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
        "if \"traverserEngines\" is set in the body, it has to be an array");
    co_return;
  }

  VPackSlice variablesSlice = querySlice.get("variables");
  if (!variablesSlice.isArray()) {
    LOG_TOPIC("6f9dc", ERR, arangodb::Logger::AQL)
        << "Invalid VelocyPack: \"variables\" attribute missing.";
    generateError(rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
                  "body must be an object with attribute \"variables\"");
    co_return;
  }

  LOG_TOPIC("f9e30", DEBUG, arangodb::Logger::AQL)
      << "Setting up cluster AQL with " << querySlice.toJson();

  VPackSlice coordinatorRebootIdSlice =
      querySlice.get(StaticStrings::AttrCoordinatorRebootId);
  VPackSlice coordinatorIdSlice =
      querySlice.get(StaticStrings::AttrCoordinatorId);
  RebootId rebootId(0);
  std::string coordinatorId;
  if (!coordinatorRebootIdSlice.isNone() || !coordinatorIdSlice.isNone()) {
    bool good = false;
    if (coordinatorRebootIdSlice.isInteger() && coordinatorIdSlice.isString()) {
      coordinatorId = coordinatorIdSlice.copyString();
      try {
        // The following will throw for negative numbers, which should not
        // happen:
        rebootId = RebootId(coordinatorRebootIdSlice.getUInt());
        good = true;
      } catch (...) {
      }
    }
    if (!good) {
      LOG_TOPIC("4251a", ERR, arangodb::Logger::AQL)
          << "Invalid VelocyPack: \"" << StaticStrings::AttrCoordinatorRebootId
          << "\" needs to be a positive number and \""
          << StaticStrings::AttrCoordinatorId
          << "\" needs to be a non-empty string";
      generateError(
          rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
          absl::StrCat("body must be an object with attribute \"",
                       StaticStrings::AttrCoordinatorRebootId, "\" and \"",
                       StaticStrings::AttrCoordinatorId, "\""));
      co_return;
    }
  }
  // Valid to not exist for upgrade scenarios!

  // Now we need to create shared_ptr<VPackBuilder>
  // That contains the old-style cluster snippet in order
  // to prepare create a Query object.
  // This old snippet is created as follows:
  //
  // {
  //   collections: [ { name: "xyz", type: "READ" }, {name: "abc", type:
  //   "WRITE"} ], initialize: false, nodes: <one of snippets[*].value>,
  //   variables: <variables slice>
  // }

  QueryOptions options(optionsSlice);
  if (options.ttl <= 0) {  // patch TTL value
    options.ttl = _queryRegistry->defaultTTL();
  }

  AccessMode::Type access = AccessMode::Type::READ;

  // TODO: technically we could change the code in prepareClusterQuery to parse
  //       the collection info directly
  // Build the collection information
  VPackBuilder collectionBuilder;
  collectionBuilder.openArray();
  for (auto lockInf : VPackObjectIterator(lockInfoSlice)) {
    if (!lockInf.value.isArray()) {
      LOG_TOPIC("1dc00", WARN, arangodb::Logger::AQL)
          << "Invalid VelocyPack: \"lockInfo." << lockInf.key.stringView()
          << "\" is required but not an array.";
      generateError(
          rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
          absl::StrCat("body must be an object with attribute: \"lockInfo.",
                       lockInf.key.stringView(),
                       "\" is required but not an array."));
      co_return;
    }
    for (VPackSlice col : VPackArrayIterator(lockInf.value)) {
      if (!col.isString()) {
        LOG_TOPIC("9e29f", WARN, arangodb::Logger::AQL)
            << "Invalid VelocyPack: \"lockInfo." << lockInf.key.stringView()
            << "\" is required but not an array.";
        generateError(
            rest::ResponseCode::BAD, TRI_ERROR_INTERNAL,
            absl::StrCat("body must be an object with attribute: \"lockInfo.",
                         lockInf.key.stringView(),
                         "\" is required but not an array."));
        co_return;
      }
      collectionBuilder.openObject();
      collectionBuilder.add("name", col);
      collectionBuilder.add("type", lockInf.key);
      collectionBuilder.close();

      constexpr std::string_view writeKey("write");
      constexpr std::string_view exclusiveKey("exclusive");

      if (!AccessMode::isWriteOrExclusive(access) &&
          lockInf.key.isEqualString(writeKey)) {
        access = AccessMode::Type::WRITE;
      } else if (!AccessMode::isExclusive(access) &&
                 lockInf.key.isEqualString(exclusiveKey)) {
        access = AccessMode::Type::EXCLUSIVE;
      }
    }
  }
  collectionBuilder.close();

  auto origin = transaction::OperationOriginAQL{"running AQL query"};
  
  // creates a StandaloneContext or a leased context
  auto q = ClusterQuery::create(
      clusterQueryId, co_await createTransactionContext(access, origin),
      std::move(options));
  TRI_ASSERT(clusterQueryId == 0 || clusterQueryId == q->id());

  VPackBufferUInt8 buffer;
  VPackBuilder answerBuilder(buffer);
  answerBuilder.openObject();
  answerBuilder.add(StaticStrings::Error, VPackValue(false));
  answerBuilder.add(StaticStrings::Code,
                    VPackValue(static_cast<int>(rest::ResponseCode::OK)));

  answerBuilder.add(StaticStrings::AqlRemoteResult,
                    VPackValue(VPackValueType::Object));
  if (clusterQueryId == 0) {
    // only return this attribute if we didn't get a query ID as input from
    // the coordinator. this will be the case for setup requests from 3.7
    // coordinators
    answerBuilder.add("queryId", VPackValue(q->id()));
  }
  // send back our own reboot id
  answerBuilder.add(StaticStrings::RebootId,
                    VPackValue(ServerState::instance()->getRebootId().value()));

  QueryAnalyzerRevisions analyzersRevision;
  auto revisionRes = analyzersRevision.fromVelocyPack(querySlice);
  if (ADB_UNLIKELY(revisionRes.fail())) {
    LOG_TOPIC("b2a37", ERR, arangodb::Logger::AQL)
        << "Failed to read ArangoSearch analyzers revision "
        << revisionRes.errorMessage();
    generateError(revisionRes);
    co_return;
  }
  auto aborter = std::make_shared<aql::QueryAborter>(q);
  q->prepareFromVelocyPack(querySlice, collectionBuilder.slice(),
                           variablesSlice, snippetsSlice, traverserSlice,
                           _request->value(StaticStrings::UserString),
                           answerBuilder, analyzersRevision, fastPath, aborter);

  answerBuilder.close();  // result
  answerBuilder.close();

  // Now set an alarm for the case that the coordinator is restarted which
  // initiated this query. In that case, we want to drop our piece here:
  if (rebootId.initialized()) {
    LOG_TOPIC("42512", TRACE, Logger::AQL)
        << "Setting RebootTracker on coordinator " << coordinatorId
        << " for query with id " << q->id();
    if (auto leaseSlice = querySlice.get("leaseId"); leaseSlice.isNumber()) {
      cluster::LeaseId leaseId{leaseSlice.getNumber<uint64_t>()};
      auto res =
          server()
              .getFeature<cluster::LeaseManagerFeature>()
              .leaseManager()
              .handoutLease(
                  PeerState{.serverId = coordinatorId, .rebootId = rebootId},
                  leaseId,
                  [query = std::weak_ptr(q)]() noexcept -> std::string {
                    if (auto q = query.lock(); q) {
                      return fmt::format(
                          "Query {} of transaction {} in database {} query: "
                          "'{}'",
                          q->id(), q->transactionId().id(), q->vocbase().name(),
                          q->queryString().string());
                    }
                    return fmt::format("Already aborted query.");
                  },
                  [queryRegistry = _queryRegistry,
                   vocbaseName = _vocbase.name(),
                   queryId = q->id()]() noexcept {
                    queryRegistry->destroyQuery(queryId,
                                                TRI_ERROR_TRANSACTION_ABORTED);
                    LOG_TOPIC("42511", DEBUG, Logger::AQL)
                        << "Query snippet destroyed as consequence of "
                           "RebootTracker for coordinator, db="
                        << vocbaseName << " queryId=" << queryId;
                  });
      if (res.fail()) {
        generateError(res.result());
        co_return;
      }
      q->addLeaseToRemoteGuard((std::move(res.get())));
    }
  }

  // query string
  std::string_view qs;
  if (auto qss = querySlice.get("qs"); qss.isString()) {
    qs = qss.stringView();
  }

  _queryRegistry->insertQuery(std::move(q), qs);

  generateResult(rest::ResponseCode::OK, std::move(buffer));
}

// PUT method for /_api/aql/<operation>/<queryId>, (internal)
// see comment in header for details
RestStatus RestAqlHandler::useQuery(std::string const& operation,
                                    std::string const& idString) {
  bool success = false;
  VPackSlice querySlice = this->parseVPackBody(success);
  if (!success) {
    return RestStatus::DONE;
  }

  if (_logContextQueryIdValue == nullptr) {
    _logContextQueryIdValue = LogContext::makeValue()
                                  .with<structuredParams::QueryId>(idString)
                                  .share();
    TRI_ASSERT(_logContextQueryIdEntry == nullptr);
    _logContextQueryIdEntry =
        LogContext::Current::pushValues(_logContextQueryIdValue);
  }

  if (!_engine) {  // the PUT verb
    TRI_ASSERT(this->state() == RestHandler::HandlerState::EXECUTE ||
               this->state() == RestHandler::HandlerState::CONTINUED);

    auto res = findEngine(idString);
    if (res.fail()) {
      if (res.is(TRI_ERROR_LOCKED)) {
        // engine is still in use, but we have enqueued a callback to be woken
        // up once it is free again
        return RestStatus::WAITING;
      }
      TRI_ASSERT(res.is(TRI_ERROR_QUERY_NOT_FOUND));
      generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_QUERY_NOT_FOUND,
                    absl::StrCat("query ID ", idString, " not found"));
      return RestStatus::DONE;
    }
    std::shared_ptr<SharedQueryState> ss = _engine->sharedState();
    ss->setWakeupHandler(withLogContext(
        [self = shared_from_this()] { return self->wakeupHandler(); }));
  }

  TRI_ASSERT(_engine != nullptr);
  TRI_ASSERT(std::to_string(_engine->engineId()) == idString);
  auto guard = _engine->getQuery().acquireLockGuard();

  if (_engine->getQuery().queryOptions().profile >= ProfileLevel::TraceOne) {
    LOG_TOPIC("1bf67", INFO, Logger::QUERIES)
        << "[query#" << _engine->getQuery().id()
        << "] remote request received: " << operation
        << " registryId=" << idString;
  }

  try {
    return handleUseQuery(operation, querySlice);
  } catch (arangodb::basics::Exception const& ex) {
    generateError(rest::ResponseCode::SERVER_ERROR, ex.code(), ex.what());
  } catch (std::exception const& ex) {
    LOG_TOPIC("d1266", ERR, arangodb::Logger::AQL)
        << "failed during use of Query: " << ex.what();

    generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR,
                  ex.what());
  } catch (...) {
    LOG_TOPIC("5a2e8", ERR, arangodb::Logger::AQL)
        << "failed during use of Query: Unknown exception occurred";

    generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR,
                  "an unknown exception occurred");
  }

  return RestStatus::DONE;
}

void RestAqlHandler::prepareExecute(bool isContinue) {
  RestVocbaseBaseHandler::prepareExecute(isContinue);
  if (_logContextQueryIdValue != nullptr) {
    TRI_ASSERT(_logContextQueryIdEntry == nullptr);
    _logContextQueryIdEntry =
        LogContext::Current::pushValues(_logContextQueryIdValue);
  }
}

// executes the handler
RestStatus RestAqlHandler::execute() {
  if (ServerState::instance()->isSingleServer()) {
    generateError(rest::ResponseCode::NOT_IMPLEMENTED,
                  TRI_ERROR_HTTP_NOT_IMPLEMENTED,
                  "this endpoint is only available in clusters");
    return RestStatus::DONE;
  }

  std::vector<std::string> const& suffixes = _request->suffixes();

  // extract the sub-request type
  rest::RequestType type = _request->requestType();

  // execute one of the CRUD methods
  switch (type) {
    case rest::RequestType::POST: {
      if (suffixes.size() != 1) {
        generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);
      } else if (suffixes[0] == "setup") {
        return waitForFuture(setupClusterQuery());
      } else {
        auto msg = absl::StrCat("Unknown POST API: ",
                                basics::StringUtils::join(suffixes, '/'));
        LOG_TOPIC("b7507", ERR, arangodb::Logger::AQL) << msg;
        generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                      std::move(msg));
      }
      break;
    }
    case rest::RequestType::PUT: {
      if (suffixes.size() != 2) {
        auto msg = absl::StrCat("Unknown PUT API: ",
                                basics::StringUtils::join(suffixes, '/'));
        LOG_TOPIC("9880a", ERR, arangodb::Logger::AQL) << msg;
        generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                      std::move(msg));
      } else {
        auto status = useQuery(suffixes[0], suffixes[1]);
        if (status == RestStatus::WAITING) {
          return status;
        }
      }
      break;
    }
    case rest::RequestType::DELETE_REQ: {
      if (suffixes.size() != 2) {
        auto msg = absl::StrCat("Unknown DELETE API: ",
                                basics::StringUtils::join(suffixes, '/'));
        LOG_TOPIC("f1993", ERR, arangodb::Logger::AQL) << msg;
        generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                      std::move(msg));
        return RestStatus::DONE;
      }
      if (suffixes[0] == "finish") {
        return handleFinishQuery(suffixes[1]);
      }

      generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_QUERY_NOT_FOUND,
                    absl::StrCat("query with id ", suffixes[1], " not found"));
      break;
    }

    default: {
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_NOT_IMPLEMENTED, "illegal method for /_api/aql");
      break;
    }
  }

  return RestStatus::DONE;
}

RestStatus RestAqlHandler::continueExecute() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  // extract the sub-request type
  rest::RequestType type = _request->requestType();

  if (type == rest::RequestType::POST) {
    // we can get here when the future produced in setupClusterQuery()
    // completes. in this case we can simply declare success
    TRI_ASSERT(suffixes.size() == 1 && suffixes[0] == "setup");
    return RestStatus::DONE;
  }
  if (type == rest::RequestType::PUT) {
    TRI_ASSERT(suffixes.size() == 2);
    return useQuery(suffixes[0], suffixes[1]);
  }
  if (type == rest::RequestType::DELETE_REQ) {
    // we can get here when the future produced in handleFinishQuery()
    // completes. in this case we can simply declare success
    TRI_ASSERT(suffixes.size() == 2 && suffixes[0] == "finish");
    return RestStatus::DONE;
  }

  generateError(
      rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
      absl::StrCat("continued non-continuable method for ",
                   GeneralRequest::translateMethod(type), " /_api/aql/",
                   basics::StringUtils::join(suffixes, "/")));

  return RestStatus::DONE;
}

void RestAqlHandler::shutdownExecute(bool isFinalized) noexcept {
  try {
    if (isFinalized && _engine != nullptr) {
      auto qId = _engine->engineId();
      _engine->sharedState()->resetWakeupHandler();
      _engine = nullptr;

      _queryRegistry->closeEngine(qId);
    }
  } catch (arangodb::basics::Exception const& ex) {
    LOG_TOPIC("f73b8", INFO, Logger::FIXME)
        << "Ignoring exception during rest handler shutdown: "
        << "[" << ex.code() << "] " << ex.message();
  } catch (std::exception const& ex) {
    LOG_TOPIC("b7335", INFO, Logger::FIXME)
        << "Ignoring exception during rest handler shutdown: " << ex.what();
  }

  if (_logContextQueryIdEntry) {
    LogContext::Current::popEntry(_logContextQueryIdEntry);
  }
  RestVocbaseBaseHandler::shutdownExecute(isFinalized);
}

// dig out the query from ID, handle errors
Result RestAqlHandler::findEngine(std::string const& idString) {
  TRI_ASSERT(_engine == nullptr);
  uint64_t qId = arangodb::basics::StringUtils::uint64(idString);

  TRI_IF_FAILURE("RestAqlHandler::killBeforeOpen") {
    auto res = _queryRegistry->openExecutionEngine(qId, {});
    // engine may not be available if the query was killed before we got here.
    // This can happen if another db server has already processed this
    // failure point, killed the query and reported back to the coordinator,
    // which then sent the finish request. If this finish request is
    // processed before the query is opened here, the query is already gone.
    if (res.ok()) {
      auto engine = res.get();
      auto queryId = engine->getQuery().id();
      _queryRegistry->destroyQuery(queryId, TRI_ERROR_QUERY_KILLED);
      _queryRegistry->closeEngine(qId);
      // Here Engine must be gone because we killed it and when closeEngine
      // drops the last reference it will be destroyed
      TRI_ASSERT(_queryRegistry->openExecutionEngine(qId, {}).is(
          TRI_ERROR_QUERY_NOT_FOUND));
    }
  }
  TRI_IF_FAILURE("RestAqlHandler::completeFinishBeforeOpen") {
    auto errorCode = TRI_ERROR_QUERY_KILLED;
    auto res = _queryRegistry->openExecutionEngine(qId, {});
    // engine may not be available due to the race described above
    if (res.ok()) {
      auto engine = res.get();
      auto queryId = engine->getQuery().id();
      // Unuse the engine, so we can abort properly
      _queryRegistry->closeEngine(qId);

      auto fut = _queryRegistry->finishQuery(queryId, errorCode);
      TRI_ASSERT(fut.isReady());
      auto query = fut.waitAndGet();
      if (query != nullptr) {
        auto f = query->finalizeClusterQuery(errorCode);
        // Wait for query to be fully finalized, as a finish call would do.
        f.wait();
        // Here Engine must be gone because we finalized it and since there
        // should not be any other references this should also destroy it.
        TRI_ASSERT(_queryRegistry->openExecutionEngine(qId, {}).is(
            TRI_ERROR_QUERY_NOT_FOUND));
      }
    }
  }
  TRI_IF_FAILURE("RestAqlHandler::prematureCommitBeforeOpen") {
    auto res = _queryRegistry->openExecutionEngine(qId, {});
    if (res.ok()) {
      auto engine = res.get();
      auto queryId = engine->getQuery().id();
      _queryRegistry->destroyQuery(queryId, TRI_ERROR_NO_ERROR);
      _queryRegistry->closeEngine(qId);
      // Here Engine could be gone
    }
  }
  auto res = _queryRegistry->openExecutionEngine(
      qId, [self = shared_from_this()]() { self->wakeupHandler(); });
  if (res.fail()) {
    return std::move(res).result();
  }

  _engine = res.get();
  TRI_ASSERT(_engine != nullptr || _engine->engineId() == qId);

  return Result{};
}

class AqlExecuteCall {
 public:
  // Deserializing factory
  static auto fromVelocyPack(VPackSlice slice) -> ResultT<AqlExecuteCall>;

  auto callStack() const noexcept -> AqlCallStack const& { return _callStack; }

 private:
  AqlExecuteCall(AqlCallStack&& callStack) : _callStack(std::move(callStack)) {}

  AqlCallStack _callStack;
};

// TODO Use the deserializer when available
auto AqlExecuteCall::fromVelocyPack(VPackSlice const slice)
    -> ResultT<AqlExecuteCall> {
  if (ADB_UNLIKELY(!slice.isObject())) {
    using namespace std::string_literals;
    return Result(
        TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
        "When deserializating AqlExecuteCall: Expected object, got "s +
            slice.typeName());
  }

  auto expectedPropertiesFound =
      std::array<std::pair<std::string_view, bool>, 1>{
          {{StaticStrings::AqlRemoteCallStack, false}}};

  std::optional<AqlCallStack> callStack;

  for (auto it : VPackObjectIterator(slice, /*useSequentialIteration*/ true)) {
    auto key = it.key.stringView();

    if (auto propIt = std::find_if(
            expectedPropertiesFound.begin(), expectedPropertiesFound.end(),
            [&key](auto const& epf) { return epf.first == key; });
        ADB_LIKELY(propIt != expectedPropertiesFound.end())) {
      TRI_ASSERT(!propIt->second);
      propIt->second = true;
    }

    if (key == StaticStrings::AqlRemoteCallStack) {
      auto maybeCallStack = AqlCallStack::fromVelocyPack(it.value);
      if (ADB_UNLIKELY(maybeCallStack.fail())) {
        return Result(
            TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            absl::StrCat(
                "When deserializating AqlExecuteCall: failed to deserialize ",
                StaticStrings::AqlRemoteCallStack, ": ",
                maybeCallStack.errorMessage()));
      }

      callStack = maybeCallStack.get();
    } else {
      LOG_TOPIC("0dd42", WARN, Logger::AQL)
          << "When deserializating AqlExecuteCall: Encountered unexpected key "
          << key;
      // If you run into this assertion during rolling upgrades after adding a
      // new attribute, remove it in the older version.
      TRI_ASSERT(false);
    }
  }

  for (auto const& it : expectedPropertiesFound) {
    if (ADB_UNLIKELY(!it.second)) {
      return Result(
          TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
          absl::StrCat("When deserializating AqlExecuteCall: missing key ",
                       it.first));
    }
  }

  TRI_ASSERT(callStack.has_value());

  return {AqlExecuteCall{std::move(callStack).value()}};
}

// handle for useQuery
RestStatus RestAqlHandler::handleUseQuery(std::string const& operation,
                                          VPackSlice querySlice) {
  VPackOptions const* opts = &VPackOptions::Defaults;
  if (_engine) {  // might be destroyed on shutdown
    opts = &_engine->getQuery().vpackOptions();
  }

  VPackBuffer<uint8_t> answerBuffer;
  VPackBuilder answerBuilder(answerBuffer);
  answerBuilder.openObject(/*unindexed*/ true);

  if (operation == StaticStrings::AqlRemoteExecute) {
    auto maybeExecuteCall = AqlExecuteCall::fromVelocyPack(querySlice);
    if (maybeExecuteCall.fail()) {
      generateError(std::move(maybeExecuteCall).result());
      return RestStatus::DONE;
    }
    TRI_IF_FAILURE("RestAqlHandler::getSome") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    auto& executeCall = maybeExecuteCall.get();

    auto items = SharedAqlItemBlockPtr{};
    auto skipped = SkipResult{};
    auto state = ExecutionState::HASMORE;

    std::string const& shardId =
        _request->header(StaticStrings::AqlShardIdHeader);

    auto const rootNodeType = _engine->root()->getPlanNode()->getType();

    // shardId is set IFF the root node is scatter or distribute
    TRI_ASSERT(shardId.empty() != (rootNodeType == ExecutionNode::SCATTER ||
                                   rootNodeType == ExecutionNode::DISTRIBUTE));

    if (shardId.empty()) {
      std::tie(state, skipped, items) =
          _engine->execute(executeCall.callStack());
    } else {
      std::tie(state, skipped, items) =
          _engine->executeForClient(executeCall.callStack(), shardId);
    }

    if (state == ExecutionState::WAITING) {
      TRI_IF_FAILURE("RestAqlHandler::killWhileWaiting") {
        _queryRegistry->destroyQuery(_engine->engineId(),
                                     TRI_ERROR_QUERY_KILLED);
      }
      return RestStatus::WAITING;
    }
    TRI_IF_FAILURE("RestAqlHandler::killWhileWritingResult") {
      _queryRegistry->destroyQuery(_engine->engineId(), TRI_ERROR_QUERY_KILLED);
    }

    auto result = AqlExecuteResult{state, skipped, std::move(items)};
    answerBuilder.add(VPackValue(StaticStrings::AqlRemoteResult));
    result.toVelocyPack(answerBuilder, opts);
    answerBuilder.add(StaticStrings::Code, VPackValue(TRI_ERROR_NO_ERROR));
  } else if (operation == "initializeCursor") {
    auto items = _engine->itemBlockManager().requestAndInitBlock(
        querySlice.get("items"));
    auto tmpRes = _engine->initializeCursor(std::move(items), /*pos*/ 0);
    if (tmpRes.first == ExecutionState::WAITING) {
      return RestStatus::WAITING;
    }
    answerBuilder.add(StaticStrings::Error, VPackValue(tmpRes.second.fail()));
    answerBuilder.add(StaticStrings::Code,
                      VPackValue(tmpRes.second.errorNumber()));
  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);
    return RestStatus::DONE;
  }

  answerBuilder.close();

  generateResult(rest::ResponseCode::OK, std::move(answerBuffer), opts);

  return RestStatus::DONE;
}

// handle query finalization for all engines
RestStatus RestAqlHandler::handleFinishQuery(std::string const& idString) {
  TRI_IF_FAILURE("Query::finishTimeout") {
    // intentionally delay the request
    std::this_thread::sleep_for(
        std::chrono::milliseconds(RandomGenerator::interval(uint32_t(1000))));
  }

  auto qid = arangodb::basics::StringUtils::uint64(idString);
  bool success = false;
  VPackSlice querySlice = this->parseVPackBody(success);
  if (!success) {
    return RestStatus::DONE;
  }

  auto errorCode =
      basics::VelocyPackHelper::getNumericValue<ErrorCode,
                                                ErrorCode::ValueType>(
          querySlice, StaticStrings::Code, TRI_ERROR_INTERNAL);

  auto f =
      _queryRegistry->finishQuery(qid, errorCode)
          .thenValue([self = shared_from_this(), this,
                      errorCode](std::shared_ptr<ClusterQuery> query) mutable
                     -> futures::Future<futures::Unit> {
            if (query == nullptr) {
              // this may be a race between query garbage collection and
              // the client  shutting down the query. it is debatable
              // whether this is an actual error if we only want to abort
              // the query...
              generateError(rest::ResponseCode::NOT_FOUND,
                            TRI_ERROR_HTTP_NOT_FOUND);
              return futures::Unit{};
            }
            // we must be the only user of this query
            TRI_ASSERT(query.use_count() == 1)
                << "Finalizing query with use_count " << query.use_count();
            return query->finalizeClusterQuery(errorCode).thenValue(
                [self = std::move(self), this,
                 q = std::move(query)](Result res) {
                  VPackBufferUInt8 buffer;
                  VPackBuilder answerBuilder(buffer);
                  answerBuilder.openObject(/*unindexed*/ true);
                  answerBuilder.add(VPackValue("stats"));

                  q->executionStatsGuard().doUnderLock(
                      [&](auto& executionStats) {
                        executionStats.toVelocyPack(
                            answerBuilder, q->queryOptions().fullCount);
                      });

                  q->warnings().toVelocyPack(answerBuilder);
                  answerBuilder.add(StaticStrings::Error,
                                    VPackValue(res.fail()));
                  answerBuilder.add(StaticStrings::Code,
                                    VPackValue(res.errorNumber()));
                  answerBuilder.close();

                  generateResult(rest::ResponseCode::OK, std::move(buffer));
                });
          });

  return waitForFuture(std::move(f));
}

RequestLane RestAqlHandler::lane() const {
  TRI_ASSERT(!ServerState::instance()->isSingleServer());

  if (ServerState::instance()->isCoordinator()) {
    // continuation requests on coordinators will get medium priority,
    // so that they don't block query parts elsewhere
    static_assert(
        PriorityRequestLane(RequestLane::CLUSTER_AQL_INTERNAL_COORDINATOR) ==
            RequestPriority::MED,
        "invalid request lane priority");
    return RequestLane::CLUSTER_AQL_INTERNAL_COORDINATOR;
  }

  if (ServerState::instance()->isDBServer()) {
    std::vector<std::string> const& suffixes = _request->suffixes();

    if (suffixes.size() == 2 && suffixes[0] == "finish") {
      // AQL shutdown requests should have medium priority, so it can release
      // locks etc. and unblock other pending requests
      static_assert(PriorityRequestLane(RequestLane::CLUSTER_AQL_SHUTDOWN) ==
                        RequestPriority::MED,
                    "invalid request lane priority");
      return RequestLane::CLUSTER_AQL_SHUTDOWN;
    }

    if (suffixes.size() == 1 && suffixes[0] == "setup") {
      return RequestLane::INTERNAL_LOW;
    }
  }

  // everything else will run with med priority
  static_assert(
      PriorityRequestLane(RequestLane::CLUSTER_AQL) == RequestPriority::MED,
      "invalid request lane priority");
  return RequestLane::CLUSTER_AQL;
}
