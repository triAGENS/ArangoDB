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

#include "QueryRegistry.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/ClusterQuery.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/Query.h"
#include "Aql/Timing.h"
#include "Basics/ReadLocker.h"
#include "Basics/WriteLocker.h"
#include "Basics/conversions.h"
#include "Basics/system-functions.h"
#include "Basics/voc-errors.h"
#include "Cluster/CallbackGuard.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "Futures/Future.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Scheduler/SchedulerFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/Status.h"

#include <utility>

using namespace arangodb;
using namespace arangodb::aql;

QueryRegistry::~QueryRegistry() {
  disallowInserts();
  destroyAll();
}

/// @brief insert
void QueryRegistry::insertQuery(std::shared_ptr<ClusterQuery> query,
                                std::string_view qs) {
  TRI_ASSERT(!ServerState::instance()->isSingleServer());

  TRI_ASSERT(query != nullptr);
  TRI_ASSERT(query->state() != QueryExecutionState::ValueType::INITIALIZATION);
  LOG_TOPIC("77778", DEBUG, arangodb::Logger::AQL)
      << "Register query with id " << query->id() << " : "
      << query->queryString();
  auto& vocbase = query->vocbase();

  if (vocbase.isDropped()) {
    // don't register any queries for dropped databases
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  QueryId qId = query->id();

  TRI_IF_FAILURE("QueryRegistryInsertException1") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // create the query info object outside of the lock
  auto p = std::make_unique<QueryInfo>(query, qs);

  TRI_IF_FAILURE("QueryRegistryInsertException2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // now insert into table of running queries
  WRITE_LOCKER(writeLocker, _lock);
  if (_disallowInserts) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }

  try {
    for (auto& engine : p->_query->snippets()) {
      if (engine->engineId() != 0) {  // skip the root snippet
        auto result = _engines.try_emplace(engine->engineId(),
                                           EngineInfo(engine.get(), p.get()));
        TRI_ASSERT(result.second);
        TRI_ASSERT(result.first->second._type == EngineType::Execution);
        p->_numEngines++;
      }
    }
    for (auto& engine : p->_query->traversers()) {
      auto result = _engines.try_emplace(engine->engineId(),
                                         EngineInfo(engine.get(), p.get()));
      TRI_ASSERT(result.second);
      TRI_ASSERT(result.first->second._type == EngineType::Graph);
      p->_numEngines++;
    }

    auto result = _queries.try_emplace(qId, std::move(p));
    if (!result.second) {
      if (result.first->second->_isTombstone) {
        // found a tombstone entry for the query id!
        // that means a concurrent thread has overtaken us and already aborted
        // the query!
        LOG_TOPIC("580c1", DEBUG, arangodb::Logger::AQL)
            << "unable to insert query " << qId
            << " into query registry because of existing tombstone";

        THROW_ARANGO_EXCEPTION_MESSAGE(result.first->second->_errorCode,
                                       "query was already aborted before");
      }
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "query with given vocbase and id already there");
    }

  } catch (...) {
    // revert engine registration
    for (auto& engine : query->snippets()) {
      _engines.erase(engine->engineId());
    }
    for (auto& engine : query->traversers()) {
      _engines.erase(engine->engineId());
    }
    // no need to revert last insert
    throw;
  }
  // we want to release the ptr before releasing the lock!
  query.reset();
}

/// @brief open
ResultT<void*> QueryRegistry::openEngine(EngineId id, EngineType type,
                                         EngineCallback callback) {
  LOG_TOPIC("8c204", DEBUG, arangodb::Logger::AQL)
      << "trying to open engine with id " << id;
  // std::cout << "Taking out query with ID " << id << std::endl;
  WRITE_LOCKER(writeLocker, _lock);

  auto it = _engines.find(id);
  if (it == _engines.end()) {
    LOG_TOPIC("c3ae4", DEBUG, arangodb::Logger::AQL)
        << "Found no engine with id " << id;
    return {TRI_ERROR_QUERY_NOT_FOUND};
  }

  EngineInfo& ei = it->second;

  if (ei._type != type) {
    LOG_TOPIC("c3af5", DEBUG, arangodb::Logger::AQL)
        << "Engine with id " << id << " has other type";
    return {TRI_ERROR_QUERY_NOT_FOUND};
  }

  if (ei._isOpen) {
    LOG_TOPIC("7c2a3", DEBUG, arangodb::Logger::AQL)
        << "Engine with id " << id << " is already open";
    if (callback) {
      ei._waitingCallbacks.emplace_back(std::move(callback));
    }
    return {TRI_ERROR_LOCKED};
  }

  ei._isOpen = true;
  if (ei._queryInfo) {
    if (ei._queryInfo->_finished) {
      ei._isOpen = false;
      return {TRI_ERROR_QUERY_NOT_FOUND};
    }
    ei._queryInfo->_numOpen++;

    LOG_TOPIC("b1cfd", TRACE, arangodb::Logger::AQL)
        << "opening engine " << id
        << ", query id: " << ei._queryInfo->_query->id()
        << ", numOpen: " << ei._queryInfo->_numOpen;
  } else {
    LOG_TOPIC("50eff", TRACE, arangodb::Logger::AQL)
        << "opening engine " << id << ", no query";
  }

  return {ei._engine};
}

traverser::BaseEngine* QueryRegistry::openGraphEngine(EngineId eid) {
  auto res = openEngine(eid, EngineType::Graph, {});
  if (res.fail()) {
    if (res.is(TRI_ERROR_LOCKED)) {
      // To be consistent with the old interface we have to throw in this case
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_LOCKED, "query with given vocbase and id is already open");
    }
    return nullptr;
  }
  return static_cast<traverser::BaseEngine*>(res.get());
}

/// @brief close
void QueryRegistry::closeEngine(EngineId engineId) {
  LOG_TOPIC("3f0c9", DEBUG, arangodb::Logger::AQL)
      << "returning engine with id " << engineId;

  // finishing the query will call back into the registry to destroy the query,
  // so this must be done outside the lock
  futures::Promise<std::shared_ptr<ClusterQuery>> finishPromise;
  std::shared_ptr<ClusterQuery> queryToFinish;

  {
    WRITE_LOCKER(writeLocker, _lock);

    auto it = _engines.find(engineId);
    if (it == _engines.end()) {
      LOG_TOPIC("97351", DEBUG, arangodb::Logger::AQL)
          << "Found no engine with id " << engineId;
      return;
    }

    EngineInfo& ei = it->second;
    if (!ei._isOpen) {
      LOG_TOPIC("45b8e", DEBUG, arangodb::Logger::AQL)
          << "engine id " << engineId << " was not open.";
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "engine with given vocbase and id is not open");
    }

    ei._isOpen = false;
    ei.scheduleCallback();

    if (ei._queryInfo) {
      TRI_ASSERT(ei._queryInfo->_numOpen > 0);
      ei._queryInfo->_numOpen--;
      bool canDestroyQuery = false;
      if (ei._queryInfo->_numOpen == 0) {
        canDestroyQuery =
            ei._queryInfo->_query->killed() || ei._queryInfo->_finished;

        if (ei._queryInfo->_finished) {
          // we were the last thread to close the engine, but the query has
          // already been marked as finished, so we are responsible to resolve
          // the future in order to actually finish the query
          queryToFinish = ei._queryInfo->_query;
          finishPromise = std::move(ei._queryInfo->_promise);
        }
      }

      LOG_TOPIC("5ecdc", TRACE, arangodb::Logger::AQL)
          << "closing engine " << engineId
          << ", query id: " << ei._queryInfo->_query->id()
          << ", numOpen: " << ei._queryInfo->_numOpen;

      if (canDestroyQuery) {
        auto queryMapIt = _queries.find(ei._queryInfo->_query->id());
        TRI_ASSERT(queryMapIt != _queries.end());
        deleteQuery(queryMapIt);
      }
    } else {
      LOG_TOPIC("ae981", TRACE, arangodb::Logger::AQL)
          << "closing engine " << engineId << ", no query";
    }
  }
  finishPromise.setValue(std::move(queryToFinish));
}

/// @brief destroy
// cppcheck-suppress virtualCallInConstructor
void QueryRegistry::destroyQuery(QueryId id, ErrorCode errorCode) {
  WRITE_LOCKER(writeLocker, _lock);

  QueryInfoMap::iterator queryMapIt = lookupQueryForFinalization(id, errorCode);
  if (queryMapIt == _queries.end()) {
    return;
  }

  if (queryMapIt->second->_numOpen == 0) {
    deleteQuery(queryMapIt);
  }
}

futures::Future<std::shared_ptr<ClusterQuery>> QueryRegistry::finishQuery(
    QueryId id, ErrorCode errorCode) {
  WRITE_LOCKER(writeLocker, _lock);

  QueryInfoMap::iterator queryMapIt = lookupQueryForFinalization(id, errorCode);
  if (queryMapIt == _queries.end()) {
    return std::shared_ptr<ClusterQuery>();
  }
  TRI_ASSERT(queryMapIt->second);
  auto& queryInfo = *queryMapIt->second;

  TRI_ASSERT(!queryInfo._finished);
  if (queryInfo._finished) {
    return std::shared_ptr<ClusterQuery>();
  }

  queryInfo._finished = true;
  if (queryInfo._numOpen > 0) {
    // we return a future for this queryInfo which will be resolved once the
    // last thread closes its engine
    return queryInfo._promise.getFuture();
  }

  auto result = queryInfo._query;
  deleteQuery(queryMapIt);
  return result;
}

auto QueryRegistry::lookupQueryForFinalization(QueryId id, ErrorCode errorCode)
    -> QueryInfoMap::iterator {
  QueryInfoMap::iterator queryMapIt = _queries.find(id);

  if (queryMapIt == _queries.end()) {
    if (errorCode != TRI_ERROR_NO_ERROR &&
        errorCode != TRI_ERROR_SHUTTING_DOWN) {
      // insert a tombstone with a higher-than-query timeout
      LOG_TOPIC("779f6", DEBUG, arangodb::Logger::AQL)
          << "inserting tombstone for query " << id << " into query registry";
      auto inserted =
          _queries
              .emplace(id, std::make_unique<QueryInfo>(
                               errorCode))
              .second;
      TRI_ASSERT(inserted);
      return _queries.end();
    }

    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "query with given id not found in query registry");
  }

  TRI_ASSERT(queryMapIt->second);
  auto& queryInfo = *queryMapIt->second;

  if (queryInfo._isTombstone) {
    _queries.erase(queryMapIt);
    return _queries.end();
  }

  if (queryInfo._numOpen > 0) {
    TRI_ASSERT(!queryInfo._isTombstone);
    // query in use by another thread/request
    if (errorCode != TRI_ERROR_NO_ERROR) {
      queryInfo._query->kill();
    }
  }
  return queryMapIt;
}

void QueryRegistry::deleteQuery(QueryInfoMap::iterator queryMapIt) {
  auto id = queryMapIt->first;
  // move query into our unique ptr, so we can process it outside
  // of the lock
  auto queryInfo = std::move(queryMapIt->second);
  TRI_ASSERT(queryInfo != nullptr);
  TRI_ASSERT(queryInfo->_numOpen == 0);

  // remove query from the table of running queries
  _queries.erase(queryMapIt);

  // remove engines
  for (auto const& engine : queryInfo->_query->snippets()) {
#ifndef ARANGODB_ENABLE_MAINTAINER_MODE
    _engines.erase(engine->engineId());
#else

    auto it = _engines.find(engine->engineId());
    if (it != _engines.end()) {
      TRI_ASSERT(it->second._queryInfo != nullptr);
      TRI_ASSERT(!it->second._isOpen);
      _engines.erase(it);
    }
#endif
  }
  for (auto& engine : queryInfo->_query->traversers()) {
    _engines.erase(engine->engineId());
  }

  queryInfo->_query->completeLeases();

  LOG_TOPIC("6756c", DEBUG, arangodb::Logger::AQL)
      << "query with id " << id << " is now destroyed";
}

/// used for a legacy shutdown
bool QueryRegistry::destroyEngine(EngineId engineId, ErrorCode errorCode) {
  QueryId qId = 0;

  {
    WRITE_LOCKER(writeLocker, _lock);

    auto it = _engines.find(engineId);
    if (it == _engines.end()) {
      LOG_TOPIC("2b5e6", DEBUG, arangodb::Logger::AQL)
          << "Found no engine with id " << engineId;
      return false;
    }

    EngineInfo& ei = it->second;
    if (ei._isOpen) {
      if (ei._queryInfo && errorCode == TRI_ERROR_QUERY_KILLED) {
        ei._queryInfo->_query->kill();
      }
      LOG_TOPIC("b342e", DEBUG, arangodb::Logger::AQL)
          << "engine id " << engineId << " is open.";
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "engine with given vocbase and id is open");
    }

    if (ei._queryInfo) {
      TRI_ASSERT(ei._queryInfo->_numEngines > 0);
      ei._queryInfo->_numEngines--;
      if (ei._queryInfo->_numEngines == 0) {  // shutdown Query
        qId = ei._queryInfo->_query->id();
      }
    }

    _engines.erase(it);
  }

  if (qId != 0) {  // simon: old shutdown case
    destroyQuery(qId, errorCode);
  }

  return true;
}

void QueryRegistry::destroy(std::string const& vocbase) {
  {
    WRITE_LOCKER(writeLocker, _lock);

    for (auto& [_, queryInfo] : _queries) {
      if (queryInfo->_query && queryInfo->_query->vocbase().name() == vocbase) {
        if (queryInfo->_numOpen > 0) {
          TRI_ASSERT(!queryInfo->_isTombstone);

          // query in use by another thread/request
          queryInfo->_query->kill();
        }
      }
    }
  }
}

/// @brief return number of registered queries, excluding tombstones
size_t QueryRegistry::numberRegisteredQueries() {
  READ_LOCKER(readLocker, _lock);
  return std::count_if(_queries.begin(), _queries.end(),
                       [](auto& v) { return !v.second->_isTombstone; });
}

/// @brief export query registry contents to velocypack
void QueryRegistry::toVelocyPack(velocypack::Builder& builder) const {
  double const now = TRI_microtime();

  READ_LOCKER(readLocker, _lock);

  for (auto const& it : _queries) {
    builder.openObject();
    builder.add("id", VPackValue(it.first));

    if (it.second != nullptr) {
      builder.add("numEngines", VPackValue(it.second->_numEngines));
      builder.add("numOpen", VPackValue(it.second->_numOpen));
      builder.add("errorCode",
                  VPackValue(static_cast<int>(it.second->_errorCode)));
      builder.add("isTombstone", VPackValue(it.second->_isTombstone));
      builder.add("finished", VPackValue(it.second->_finished));
      builder.add("queryString", VPackValue(it.second->_queryString));

      auto const* query = it.second->_query.get();
      if (query != nullptr) {
        builder.add("id", VPackValue(query->id()));
        builder.add("transactionId", VPackValue(query->transactionId().id()));
        builder.add("database", VPackValue(query->vocbase().name()));
        builder.add("collections", VPackValue(VPackValueType::Array));
        query->collections().visit(
            [&builder](std::string const& name, aql::Collection const& c) {
              builder.openObject();
              builder.add("id", VPackValue(c.id().id()));
              builder.add("name", VPackValue(c.name()));
              builder.add("access",
                          VPackValue(AccessMode::typeString(c.accessType())));
              builder.close();
              return true;
            });
        builder.close();  // collections
        builder.add("killed", VPackValue(query->killed()));

        double const elapsed = elapsedSince(query->startTime());
        auto timeString =
            TRI_StringTimeStamp(now - elapsed, Logger::getUseLocalTime());
        builder.add("startTime", VPackValue(timeString));
        builder.add("isModificationQuery",
                    VPackValue(query->isModificationQuery()));
      }

      if (it.second->_numEngines > 0) {
        builder.add("engines", VPackValue(VPackValueType::Array));
        for (auto const& e : _engines) {
          if (e.second._queryInfo != it.second.get()) {
            continue;
          }
          builder.openObject();
          builder.add("engineId", VPackValue(e.first));
          builder.add("isOpen", VPackValue(e.second._isOpen));
          builder.add("type", VPackValue(e.second._type == EngineType::Graph
                                             ? "graph"
                                             : "execution"));
          builder.add("waitingCallbacks",
                      VPackValue(e.second._waitingCallbacks.size()));
          builder.close();
        }
        builder.close();
      }
    }
    builder.add("serverId", VPackValue(ServerState::instance()->getId()));
    builder.close();
  }
}

/// @brief for shutdown, we need to shut down all queries:
void QueryRegistry::destroyAll() try {
  std::vector<QueryId> allQueries;
  {
    READ_LOCKER(readlock, _lock);
    allQueries.reserve(_queries.size());
    for (auto& q : _queries) {
      allQueries.emplace_back(q.first);
    }
  }
  for (auto& qid : allQueries) {
    try {
      LOG_TOPIC("df275", DEBUG, arangodb::Logger::AQL)
          << "Timeout for query with id " << qid << " due to shutdown";
      destroyQuery(qid, TRI_ERROR_SHUTTING_DOWN);
    } catch (...) {
      // ignore any errors here
    }
  }

  size_t count = numberRegisteredQueries();
  if (count > 0) {
    LOG_TOPIC("43bf8", INFO, arangodb::Logger::AQL)
        << "number of remaining queries in query registry at shutdown: "
        << count;
  }
} catch (std::exception const& ex) {
  LOG_TOPIC("30bdb", INFO, arangodb::Logger::AQL)
      << "caught exception during shutdown of query registry: " << ex.what();
}

void QueryRegistry::disallowInserts() {
  WRITE_LOCKER(writelock, _lock);
  _disallowInserts = true;
  // from here on, there shouldn't be any more inserts into the registry
}

/// use on coordinator to register snippets
void QueryRegistry::registerSnippets(SnippetList const& snippets) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  WRITE_LOCKER(guard, _lock);
  if (_disallowInserts) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  for (auto& engine : snippets) {
    if (engine->engineId() != 0) {  // skip the root snippet
      auto result = _engines.try_emplace(engine->engineId(),
                                         EngineInfo(engine.get(), nullptr));
      TRI_ASSERT(result.second);
    }
  }
}

void QueryRegistry::unregisterSnippets(SnippetList const& snippets) noexcept {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  int iterations = 0;

  while (true) {
    WRITE_LOCKER(guard, _lock);
    size_t remain = snippets.size();
    for (auto& engine : snippets) {
      auto it = _engines.find(engine->engineId());
      if (it == _engines.end()) {
        remain--;
        continue;
      }
      if (it->second._isOpen) {  // engine still in use
        continue;
      }
      _engines.erase(it);
      remain--;
    }
    guard.unlock();
    if (remain == 0) {
      break;
    }
    if (iterations == 0) {
      LOG_TOPIC("33cfb", DEBUG, arangodb::Logger::AQL)
          << remain << " engine snippet(s) still in use on query shutdown";
    } else if (iterations == 100) {
      LOG_TOPIC("df7c7", WARN, arangodb::Logger::AQL)
          << remain << " engine snippet(s) still in use on query shutdown";
    }
    ++iterations;

    std::this_thread::yield();
  }
}

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
bool QueryRegistry::queryIsRegistered(QueryId id) {
  READ_LOCKER(readLocker, _lock);
  return _queries.contains(id);
}
#endif

/// @brief constructor for a regular query
QueryRegistry::QueryInfo::QueryInfo(std::shared_ptr<ClusterQuery> query,
                                    std::string_view qs)
    : _query(std::move(query)),
      _numEngines(0),
      _numOpen(0),
      _queryString(qs),
      _errorCode(TRI_ERROR_NO_ERROR),
      _isTombstone(false) {}

/// @brief constructor for a tombstone
QueryRegistry::QueryInfo::QueryInfo(ErrorCode errorCode)
    : _query(nullptr),
      _numEngines(0),
      _numOpen(0),
      _errorCode(errorCode),
      _isTombstone(true) {}

QueryRegistry::QueryInfo::~QueryInfo() {
  if (!_promise.isFulfilled()) {
    // we just set a dummy value to avoid abandoning the promise, because this
    // makes it much more difficult to debug cases where we _must not_ abandon a
    // promise
    _promise.setValue(std::shared_ptr<ClusterQuery>{nullptr});
  }
}

QueryRegistry::EngineInfo::~EngineInfo() {
  // If we still have requests waiting for this engine, we need to wake schedule
  // them so they can abort properly.
  while (!_waitingCallbacks.empty()) {
    scheduleCallback();
  }
}

void QueryRegistry::EngineInfo::scheduleCallback() {
  if (!_waitingCallbacks.empty()) {
    // we have at least one request handler waiting for this engine.
    // schedule the callback to be executed so request handler can continue
    auto callback = std::move(_waitingCallbacks.front());
    _waitingCallbacks.pop_front();
    SchedulerFeature::SCHEDULER->queue(
        RequestLane::CLUSTER_AQL_INTERNAL_COORDINATOR, std::move(callback));
  }
}
