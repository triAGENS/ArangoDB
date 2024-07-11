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

#pragma once

#include "Aql/AqlItemBlockManager.h"
#include "Aql/BindParameters.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExecutionState.h"
#include "Aql/ExecutionStats.h"
#include "Aql/QueryContext.h"
#include "Aql/QueryExecutionState.h"
#include "Aql/QueryResult.h"
#include "Basics/Guarded.h"
#ifdef USE_V8
#include "Aql/QueryResultV8.h"
#endif
#include "Aql/QueryString.h"
#include "Basics/ResourceUsage.h"
#include "Scheduler/SchedulerFeature.h"
#ifdef USE_V8
#include "V8Server/V8Executor.h"
#endif

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

struct TRI_vocbase_t;

namespace arangodb {

class CollectionNameResolver;
class LogicalDataSource;

namespace transaction {
class Context;
class Methods;
}  // namespace transaction
namespace aql {

class Ast;
struct AstNode;
class ExecutionEngine;
struct ExecutionStats;
struct QueryAborter;
struct QueryCacheResultEntry;
struct QueryProfile;
class SharedQueryState;

/// @brief an AQL query
class Query : public QueryContext, public std::enable_shared_from_this<Query> {
 protected:
  /// @brief internal constructor, Used to construct a full query or a
  /// ClusterQuery
  Query(QueryId id, std::shared_ptr<transaction::Context> ctx,
        QueryString queryString,
        std::shared_ptr<velocypack::Builder> bindParameters,
        QueryOptions options, std::shared_ptr<SharedQueryState> sharedState);

  /// Used to construct a full query. the constructor is protected to ensure
  /// that call sites only create Query objects using the `create` factory
  /// method
  Query(std::shared_ptr<transaction::Context> ctx, QueryString queryString,
        std::shared_ptr<velocypack::Builder> bindParameters,
        QueryOptions options, Scheduler* scheduler);

  ~Query() override;

  void destroy();

 public:
  Query(Query const&) = delete;
  Query& operator=(Query const&) = delete;

  /// @brief factory method for creating a query. this must be used to
  /// ensure that Query objects are always created using shared_ptrs.
  static std::shared_ptr<Query> create(
      std::shared_ptr<transaction::Context> ctx, QueryString queryString,
      std::shared_ptr<velocypack::Builder> bindParameters,
      QueryOptions options = {},
      Scheduler* scheduler = SchedulerFeature::SCHEDULER);

  constexpr static uint64_t DontCache = 0;

  /// @brief return the user that started the query
  std::string const& user() const final;

  /// @brief whether or not the query is killed
  bool killed() const final;

  /// @brief set the query to killed
  void kill();

  void setKillFlag();

  /// @brief setter and getter methods for the query lockTimeout.
  void setLockTimeout(double timeout) noexcept final;
  double getLockTimeout() const noexcept final;

  QueryString const& queryString() const { return _queryString; }

  /// @brief the query's transaction id. returns 0 if no transaction
  /// has been assigned to the query yet. use this only for informational
  /// purposes
  TransactionId transactionId() const noexcept;

  /// @brief return the start time of the query (steady clock value)
  double startTime() const noexcept;

  /// @brief return the total execution time of the query (until
  /// the start of finalize)
  double executionTime() const noexcept;

  /// @brief make sure that the query execution time is set.
  /// only the first call to this function will set the time.
  /// every following call will be ignored.
  void ensureExecutionTime() noexcept;

  void prepareQuery(std::shared_ptr<QueryAborter> queryAborter);

  /// @brief execute an AQL query
  ExecutionState execute(std::shared_ptr<QueryAborter> aborter,
                         QueryResult& res);

  /// @brief execute an AQL query and block this thread in case we
  ///        need to wait.
  QueryResult executeSync(std::shared_ptr<QueryAborter> aborter);

#ifdef USE_V8
  /// @brief execute an AQL query
  /// may only be called with an active V8 handle scope
  QueryResultV8 executeV8(v8::Isolate* isolate,
                          std::shared_ptr<QueryAborter> aborter);
#endif

  /// @brief Enter finalization phase and do cleanup.
  /// Sets `warnings`, `stats`, `profile`, timings and does the cleanup.
  /// Only use directly for a streaming query, rather use `execute(...)`
  ExecutionState finalize(velocypack::Builder& extras);

  /// @brief parse an AQL query
  QueryResult parse();

  /// @brief explain an AQL query
  QueryResult explain();

  /// @brief prepare a query out of some velocypack data.
  /// only to be used on single server or coordinator.
  /// never call this on a DB server!
  void prepareFromVelocyPack(velocypack::Slice querySlice,
                             velocypack::Slice collections,
                             velocypack::Slice variables,
                             velocypack::Slice snippets,
                             QueryAnalyzerRevisions const& analyzersRevision,
                             std::shared_ptr<QueryAborter> queryAborter);

  /// @brief whether or not a query is a modification query
  bool isModificationQuery() const noexcept final;

  bool isAsyncQuery() const noexcept final;

  /// @brief enter a V8 executor
  void enterV8Executor() final;

  /// @brief exits a V8 executor
  void exitV8Executor() final;

  /// @brief check if the query has a V8 executor ready for use
  bool hasEnteredV8Executor() const final {
#ifdef USE_V8
    return (_executorOwnedByExterior || _v8Executor != nullptr);
#else
    return false;
#endif
  }

#ifdef USE_V8
  void runInV8ExecutorContext(std::function<void(v8::Isolate*)> const& cb);
#endif

  /// @brief return the final query result status code (0 = no error,
  /// > 0 = error, one of TRI_ERROR_...)
  ErrorCode resultCode() const noexcept;

  /// @brief return the bind parameters as passed by the user
  std::shared_ptr<velocypack::Builder> bindParameters() const {
    return _bindParameters.builder();
  }

  /// @brief return the query's shared state
  std::shared_ptr<SharedQueryState> sharedState() const;

  ExecutionEngine* rootEngine() const;

  QueryOptions const& queryOptions() const final { return _queryOptions; }

  QueryOptions& queryOptions() noexcept final { return _queryOptions; }

  /// @brief pass-thru a resolver object from the transaction context
  CollectionNameResolver const& resolver() const final;

  /// @brief create a transaction::Context
  std::shared_ptr<transaction::Context> newTrxContext() const final;

  velocypack::Options const& vpackOptions() const final;

  transaction::Methods& trxForOptimization() override;

  void addIntermediateCommits(uint64_t value);

#ifdef ARANGODB_USE_GOOGLE_TESTS
  ExecutionPlan* plan() const {
    if (_plans.size() == 1) {
      return _plans[0].get();
    }
    return nullptr;
  }
  void initForTests();
  void initTrxForTests();
#endif

  AqlItemBlockManager& itemBlockManager() { return _itemBlockManager; }

  SnippetList const& snippets() const { return _snippets; }
  SnippetList& snippets() { return _snippets; }
  ServerQueryIdList& serverQueryIds() { return _serverQueryIds; }
  Guarded<ExecutionStats>& executionStatsGuard() { return _execStats; }

  // Debug method to kill a query at a specific position
  // during execution. It internally asserts that the query
  // is actually visible through other APIS (e.g. current queries)
  // so user actually has a chance to kill it here.
  void debugKillQuery() final;

  bool allowDirtyReads() const noexcept { return _allowDirtyReads; }

  /// @brief convert query bind parameters to a string representation
  void stringifyBindParameters(std::string& out, std::string_view prefix,
                               size_t maxLength) const;

  /// @brief convert query data sources to a string representation
  void stringifyDataSources(std::string& out, std::string_view prefix) const;

  /// @brief extract query string up to maxLength bytes. if show is false,
  /// returns "<hidden>" regardless of maxLength
  std::string extractQueryString(size_t maxLength, bool show) const;

  auto completeLeases() -> void;

 protected:
  /// @brief initializes the query
  void init(bool createProfile);

  void registerQueryInTransactionState();
  void unregisterQueryInTransactionState() noexcept;

  /// @brief calculate a hash for the query, once
  uint64_t hash();

  /// @brief prepare an AQL query, this is a preparation for execute, but
  /// execute calls it internally. The purpose of this separate method is
  /// to be able to only prepare a query from VelocyPack and then store it
  /// in the QueryRegistry.
  std::unique_ptr<ExecutionPlan> preparePlan();

  /// @brief calculate a hash value for the query string and bind
  /// parameters
  uint64_t calculateHash() const;

  /// @brief whether or not the query cache can be used for the query
  bool canUseQueryCache() const;

  /// @brief enter a new state
  void enterState(QueryExecutionState::ValueType);

  /// @brief cleanup plan and engine for current query can issue WAITING
  ExecutionState cleanupPlanAndEngine(ErrorCode errorCode);

  void unregisterSnippets();

 private:
  ExecutionState cleanupTrxAndEngines(ErrorCode errorCode);

  // @brief injects vertex collections into all types of graph nodes:
  // ExecutionNode::TRAVERSAL, ExecutionNode::SHORTEST_PATH and
  // ExecutionNode::ENUMERATE_PATHS - in case the GraphNode does not
  // contain a vertex collection yet. This can happen e.g. during
  // anonymous traversal.
  void injectVertexCollectionIntoGraphNodes(ExecutionPlan& plan);

  // log the start of a query (trace mode only)
  void logAtStart();

  // log the end of a query (warnings only)
  void logAtEnd(QueryResult const& queryResult) const;

  enum class ExecutionPhase { INITIALIZE, EXECUTE, FINALIZE };

 protected:
  AqlItemBlockManager _itemBlockManager;

  /// @brief the actual query string
  QueryString _queryString;

  /// collect execution stats, contains aliases
  Guarded<ExecutionStats> _execStats;

  /// @brief transaction context to use for this query
  std::shared_ptr<transaction::Context> _transactionContext;

  /// @brief shared query state
  std::shared_ptr<SharedQueryState> _sharedState;

#ifdef USE_V8
  /// @brief the currently used V8 executor
  V8Executor* _v8Executor;
#endif

  /// @brief bind parameters for the query
  BindParameters _bindParameters;

  /// @brief parsed query options
  QueryOptions _queryOptions;

  /// @brief first one should be the local one
  SnippetList _snippets;
  ServerQueryIdList _serverQueryIds;

  /// @brief query execution profile
  std::unique_ptr<QueryProfile> _queryProfile;

  /// @brief the ExecutionPlan object, if the query is prepared
  std::vector<std::unique_ptr<ExecutionPlan>> _plans;

  /// plan serialized before instantiation, used for query profiling
  std::unique_ptr<velocypack::UInt8Buffer> _planSliceCopy;

  /// @brief the transaction object, in a distributed query every part of
  /// the query has its own transaction object. The transaction object is
  /// created in the prepare method.
  std::unique_ptr<transaction::Methods> _trx;

  /// @brief query cache entry built by the query
  /// only populated when the query has generated its result(s) and before
  /// storing the cache entry in the query cache
  std::unique_ptr<QueryCacheResultEntry> _cacheEntry;

  /// @brief query start time (steady clock value)
  double const _startTime;

  /// @brief query end time (steady clock value), only set once finalize()
  /// is reached
  double _endTime;

  /// @brief total memory used for building the (partial) result
  size_t _resultMemoryUsage;

  /// @brief total memory used for the velocypack data of an execution plan
  size_t _planMemoryUsage;

  /// @brief hash for this query. will be calculated only once when needed
  mutable uint64_t _queryHash = DontCache;

  enum class ShutdownState : uint8_t { None = 0, InProgress = 2, Done = 4 };

  // atomic used because kill() might be called concurrently
  std::atomic<ShutdownState> _shutdownState;

  /// Track in which phase of execution we are, in order to implement
  /// repeatability.
  ExecutionPhase _executionPhase;

  /// @brief return the final query result status code (0 = no error,
  /// > 0 = error, one of TRI_ERROR_...)
  std::optional<ErrorCode> _resultCode;

  /// @brief user that started the query
  std::string _user;

#ifdef USE_V8
  /// @brief whether or not someone else has acquired a V8 executor for us
  bool const _executorOwnedByExterior;

  /// @brief set if we are inside a JS transaction
  bool const _embeddedQuery;

  /// @brief whether or not the transaction executor was registered
  /// in a v8 executor
  bool _registeredInV8Executor;
#endif

  /// @brief whether or not the hash was already calculated
  bool _queryHashCalculated;

  bool _registeredQueryInTrx;

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  // Intentionally initialized here to not
  // be present in production constructors
  // Indicator if a query was already killed
  // via a debug failure. This should not
  // retrigger a kill.
  bool _wasDebugKilled = false;
#endif

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  bool _wasDestroyed = false;
#endif

  bool _allowDirtyReads;  // this is set from the information in the
                          // transaction, it is valid and remains valid
                          // once `preparePlan` has run and can be queried
                          // until the query object is gone!

  /// @brief was this query killed (can only be set once)
  std::atomic<bool> _queryKilled;
};

}  // namespace aql
}  // namespace arangodb
