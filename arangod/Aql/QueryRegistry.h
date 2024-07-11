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

#pragma once

#include "Aql/types.h"
#include "Basics/ErrorCode.h"
#include "Basics/ResultT.h"
#include "Basics/ReadWriteLock.h"
#include "Futures/Promise.h"

#include <deque>
#include <string_view>
#include <unordered_map>

namespace arangodb {
namespace aql {
class ExecutionEngine;
class ClusterQuery;

/// manages cluster queries and engines
class QueryRegistry {
 public:
  explicit QueryRegistry(double defTTL)
      : _defaultTTL(defTTL), _disallowInserts(false) {}

  TEST_VIRTUAL ~QueryRegistry();

 public:
  enum class EngineType : uint8_t { Execution = 1, Graph = 2 };

  using EngineCallback = std::function<void()>;

  /// @brief insert, this inserts the query <query> for the vocbase <vocbase>
  /// and the id <id> into the registry. It is in error if there is already
  /// a query for this <vocbase> and <id> combination and an exception will
  /// be thrown in that case.
  /// With keepLease == true the query will be kept open and it is guaranteed
  /// that the caller can continue to use it exclusively.
  /// This is identical to an atomic sequence of insert();open();
  void insertQuery(std::shared_ptr<ClusterQuery> query, std::string_view qs);

  /// @brief open, find a engine in the registry, if none is found, a nullptr
  /// is returned, otherwise, ownership of the query is transferred to the
  /// caller, however, the registry retains the entry and will open will
  /// succeed only once. If an already open query with the given id is
  /// found, an exception is thrown.
  /// An open query can directly be destroyed by the destroy method.
  /// Note that an open query will not expire, so users should please
  /// protect against leaks. If an already open query is found, an exception
  /// is thrown.
  ResultT<void*> openEngine(EngineId eid, EngineType type,
                            EngineCallback callback);
  ResultT<ExecutionEngine*> openExecutionEngine(EngineId eid,
                                                EngineCallback callback) {
    auto res = openEngine(eid, EngineType::Execution, std::move(callback));
    if (res.fail()) {
      return std::move(res).result();
    }
    return static_cast<ExecutionEngine*>(res.get());
  }

  traverser::BaseEngine* openGraphEngine(EngineId eid);

  /// @brief close, return a query to the registry, if the query is not found,
  /// an exception is thrown. If the ttl is negative (the default is), the
  /// original ttl is taken again.
  void closeEngine(EngineId eId);

  /// @brief destroy, this removes the entry from the registry and calls
  /// delete on the Query*. It is allowed to call this regardless of whether
  /// the query is open or closed. No check is performed that this call comes
  /// from the same thread that has opened it! Note that if the query is
  /// "open", then this will set the "killed" flag in the query and do not
  /// more.
  /// if the ignoreOpened flag is set, it means the query will be shut down
  /// and removed regardless if it is in use by anything else. this is only
  /// safe to call if the current thread is currently using the query itself
  // cppcheck-suppress virtualCallInConstructor
  void destroyQuery(QueryId id, ErrorCode errorCode);

  futures::Future<std::shared_ptr<ClusterQuery>> finishQuery(
      QueryId id, ErrorCode errorCode);

  /// used for a legacy shutdown
  bool destroyEngine(EngineId engineId, ErrorCode errorCode);

  /// @brief destroy all queries for the specified database. this can be used
  /// when the database gets dropped
  void destroy(std::string const& vocbase);

  /// @brief return number of registered queries
  size_t numberRegisteredQueries();

  /// @brief export query registry contents to velocypack
  void toVelocyPack(velocypack::Builder& builder) const;

  /// @brief for shutdown, we need to shut down all queries:
  void destroyAll();

  /// @brief from here on, disallow entering new queries into the registry
  void disallowInserts();

  /// use on coordinator to register snippets
  void registerSnippets(SnippetList const&);
  void unregisterSnippets(SnippetList const&) noexcept;

  /// @brief return the default TTL value
  TEST_VIRTUAL double defaultTTL() const { return _defaultTTL; }

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  bool queryIsRegistered(QueryId id);
#endif

 private:
  /// @brief a struct for all information regarding one query in the registry
  struct QueryInfo final {
    /// @brief constructor for a regular query entry
    QueryInfo(std::shared_ptr<ClusterQuery> query, std::string_view qs);

    /// @brief constructor for a tombstone entry
    explicit QueryInfo(ErrorCode errorCode);
    ~QueryInfo();

    std::shared_ptr<ClusterQuery> _query;  // the actual query pointer

    /// @brief promise to finish a query that was still active when we
    /// received the finish request
    futures::Promise<std::shared_ptr<ClusterQuery>> _promise;

    size_t _numEngines;  // used for legacy shutdown
    size_t _numOpen;

    std::string _queryString;  // can be empty

    ErrorCode _errorCode;
    bool const _isTombstone;
    bool _finished = false;
  };

  struct EngineInfo final {
    EngineInfo(EngineInfo const&) = delete;
    EngineInfo& operator=(EngineInfo const&) = delete;

    EngineInfo(EngineInfo&& other)
        : _engine(std::move(other._engine)),
          _queryInfo(std::move(other._queryInfo)),
          _type(other._type),
          _isOpen(other._isOpen) {}
    EngineInfo& operator=(EngineInfo&& other) = delete;

    EngineInfo(ExecutionEngine* en, QueryInfo* qi)
        : _engine(en),
          _queryInfo(qi),
          _type(EngineType::Execution),
          _isOpen(false) {}
    EngineInfo(traverser::BaseEngine* en, QueryInfo* qi)
        : _engine(en),
          _queryInfo(qi),
          _type(EngineType::Graph),
          _isOpen(false) {}

    ~EngineInfo();

    void scheduleCallback();

    void* _engine;
    QueryInfo* _queryInfo;
    const EngineType _type;
    bool _isOpen;

    // list of callbacks from handlers that are waiting for the engine to become
    // available
    std::deque<EngineCallback> _waitingCallbacks;
  };

  using QueryInfoMap = std::unordered_map<QueryId, std::unique_ptr<QueryInfo>>;

  auto lookupQueryForFinalization(QueryId id, ErrorCode errorCode)
      -> QueryInfoMap::iterator;
  void deleteQuery(QueryInfoMap::iterator queryMapIt);

  /// @brief _queries, the actual map of maps for the registry
  /// maps from vocbase name to list queries
  QueryInfoMap _queries;

  std::unordered_map<EngineId, EngineInfo> _engines;

  /// @brief _lock, the read/write lock for access
  basics::ReadWriteLock mutable _lock;

  /// @brief the default TTL value
  double const _defaultTTL;

  bool _disallowInserts;
};

}  // namespace aql
}  // namespace arangodb
