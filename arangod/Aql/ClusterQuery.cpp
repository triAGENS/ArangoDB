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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "ClusterQuery.h"

#include "Aql/Ast.h"
#include "Aql/AqlTransaction.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/Timing.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryRegistry.h"
#include "Aql/QueryProfile.h"
#include "Aql/SharedQueryState.h"
#include "Basics/ScopeGuard.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::aql;

// Wait 2s to get the Lock in FastPath, otherwise assume dead-lock.
constexpr double kFastPathLockTimeout = 2.0;

ClusterQuery::ClusterQuery(QueryId id,
                           std::shared_ptr<transaction::Context> ctx,
                           QueryOptions options)
    : Query{id,
            ctx,
            {},
            /*bindParams*/ nullptr,
            std::move(options),
            /*sharedState*/ ServerState::instance()->isDBServer()
                ? nullptr
                : std::make_shared<SharedQueryState>(ctx->vocbase().server())} {
}

ClusterQuery::~ClusterQuery() {
  try {
    _traversers.clear();
  } catch (...) {
  }
}

/// @brief factory method for creating a cluster query. this must be used to
/// ensure that ClusterQuery objects are always created using shared_ptrs.
std::shared_ptr<ClusterQuery> ClusterQuery::create(
    QueryId id, std::shared_ptr<transaction::Context> ctx,
    QueryOptions options) {
  // workaround to enable make_shared on a class with a protected constructor
  struct MakeSharedQuery final : ClusterQuery {
    MakeSharedQuery(QueryId id, std::shared_ptr<transaction::Context> ctx,
                    QueryOptions options)
        : ClusterQuery{id, std::move(ctx), std::move(options)} {}

    ~MakeSharedQuery() final {
      // Destroy this query, otherwise it's still
      // accessible while the query is being destructed,
      // which can result in a data race on the vptr
      destroy();
    }
  };
  TRI_ASSERT(ctx != nullptr);
  return std::make_shared<MakeSharedQuery>(id, std::move(ctx),
                                           std::move(options));
}

void ClusterQuery::buildTraverserEngines(velocypack::Slice traverserSlice,
                                         velocypack::Builder& answerBuilder) {
  TRI_ASSERT(answerBuilder.isOpenObject());

  if (traverserSlice.isArray()) {
    // used to be RestAqlHandler::registerTraverserEngines
    answerBuilder.add("traverserEngines", VPackValue(VPackValueType::Array));
    for (auto te : VPackArrayIterator(traverserSlice)) {
      auto engine = traverser::BaseEngine::buildEngine(_vocbase, *this, te);
      answerBuilder.add(VPackValue(engine->engineId()));
      _traversers.emplace_back(std::move(engine));
    }
    answerBuilder.close();  // traverserEngines
  }
}

/// @brief prepare a query out of some velocypack data.
/// only to be used on a DB server.
/// never call this on a single server or coordinator!
void ClusterQuery::prepareFromVelocyPack(
    velocypack::Slice querySlice, velocypack::Slice collections,
    velocypack::Slice variables, velocypack::Slice snippets,
    velocypack::Slice traverserSlice, std::string const& user,
    velocypack::Builder& answerBuilder,
    QueryAnalyzerRevisions const& analyzersRevision, bool fastPathLocking,
    std::shared_ptr<QueryAborter> queryAborter) {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  LOG_TOPIC("45493", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " ClusterQuery::prepareFromVelocyPack"
      << " this: " << (uintptr_t)this;

  // track memory usage
  ResourceUsageScope scope(*_resourceMonitor);
  scope.increase(querySlice.byteSize() + collections.byteSize() +
                 variables.byteSize() + snippets.byteSize() +
                 traverserSlice.byteSize());

  _planMemoryUsage += scope.trackedAndSteal();

  init(/*createProfile*/ false);

  if (auto val = querySlice.get("isModificationQuery"); val.isTrue()) {
    _ast->setContainsModificationNode();
  }
  if (auto val = querySlice.get("isAsyncQuery"); val.isTrue()) {
    _ast->setContainsParallelNode();
  }

  enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);

  ExecutionPlan::getCollectionsFromVelocyPack(_collections, collections);
  _ast->variables()->fromVelocyPack(variables);
  // creating the plan may have produced some collections
  // we need to add them to the transaction now (otherwise the query will fail)

  TRI_ASSERT(_trx == nullptr);
  // needs to be created after the AST collected all collections
  std::unordered_set<std::string> inaccessibleCollections;
#ifdef USE_ENTERPRISE
  if (_queryOptions.transactionOptions.skipInaccessibleCollections) {
    inaccessibleCollections = _queryOptions.inaccessibleCollections;
  }

  waitForSatellites();
#endif

  _trx = AqlTransaction::create(_transactionContext, _collections,
                                _queryOptions.transactionOptions,
                                std::move(inaccessibleCollections));

  // create the transaction object, but do not start the transaction yet
  _trx->addHint(
      transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel
  _trx->state()->acceptAnalyzersRevision(analyzersRevision);
  _trx->setUsername(user);

  double origLockTimeout = _trx->state()->options().lockTimeout;
  if (fastPathLocking) {
    _trx->state()->options().lockTimeout = kFastPathLockTimeout;
  }

  Result res = _trx->begin();
  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  // restore original lock timeout
  _trx->state()->options().lockTimeout = origLockTimeout;

  TRI_IF_FAILURE("Query::setupLockTimeout") {
    if (!_trx->state()->isReadOnlyTransaction() &&
        RandomGenerator::interval(uint32_t(100)) >= 95) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_LOCK_TIMEOUT);
    }
  }

  _collections.visit([&](std::string const&, aql::Collection const& c) -> bool {
    // this code will only execute on leaders
    _trx->state()->trackShardRequest(*_trx->resolver(), _vocbase.name(),
                                     c.name(), user, c.accessType(), "aql");
    return true;
  });

  enterState(QueryExecutionState::ValueType::PARSING);

  bool const planRegisters = !_queryString.empty();
  auto instantiateSnippet = [&](velocypack::Slice snippet) {
    auto plan = ExecutionPlan::instantiateFromVelocyPack(_ast.get(), snippet);
    TRI_ASSERT(plan != nullptr);

    ExecutionEngine::instantiateFromPlan(*this, *plan, planRegisters,
                                         queryAborter);
    _plans.push_back(std::move(plan));
  };

  TRI_ASSERT(answerBuilder.isOpenObject());
  answerBuilder.add("snippets", VPackValue(VPackValueType::Object));
  for (auto pair : VPackObjectIterator(snippets, /*sequential*/ true)) {
    instantiateSnippet(pair.value);

    TRI_ASSERT(!_snippets.empty());
    TRI_ASSERT(_snippets.back()->engineId() != 0);

    answerBuilder.add(pair.key.stringView(),
                      VPackValue(std::to_string(_snippets.back()->engineId())));
  }
  answerBuilder.close();  // snippets

  if (!_snippets.empty()) {
    registerQueryInTransactionState();
  }

  buildTraverserEngines(traverserSlice, answerBuilder);

  enterState(QueryExecutionState::ValueType::EXECUTION);
}

futures::Future<Result> ClusterQuery::finalizeClusterQuery(
    ErrorCode errorCode) {
  TRI_ASSERT(_trx);
  TRI_ASSERT(ServerState::instance()->isDBServer());

  // technically there is no need for this in DBServers, but it should
  // be good practice to prevent the other cleanup code from running
  ShutdownState exp = ShutdownState::None;
  if (!_shutdownState.compare_exchange_strong(exp, ShutdownState::InProgress)) {
    return Result{TRI_ERROR_INTERNAL,
                  "query already finalized"};  // someone else got here
  }

  LOG_TOPIC("fc33c", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::finalizeSnippets: before _trx->commit, errorCode: "
      << errorCode << ", this: " << (uintptr_t)this;

  enterState(QueryExecutionState::ValueType::FINALIZATION);

  for (auto& engine : _snippets) {  // make sure all snippets are unused
    engine->sharedState()->invalidate();
    executionStatsGuard().doUnderLock([&](auto& executionStats) {
      engine->collectExecutionStats(executionStats);
    });
  }

  // Use async API, commit on followers sends a request
  futures::Future<Result> finishResult(Result{});
  if (_trx->status() == transaction::Status::RUNNING) {
    if (errorCode == TRI_ERROR_NO_ERROR) {
      // no error. we need to commit the transaction
      finishResult = _trx->commitAsync();
    } else {
      // got an error. we need to abort the transaction
      finishResult = _trx->abortAsync();
    }
  }

  return std::move(finishResult).thenValue([this](Result res) -> Result {
    LOG_TOPIC("8ea28", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime) << " Query::finalizeSnippets post commit()"
        << " this: " << (uintptr_t)this;

    // Query commited/aborted. We can clear the leases.
    completeLeases();

    executionStatsGuard().doUnderLock([&](auto& executionStats) {
      executionStats.requests += _numRequests.load(std::memory_order_relaxed);
      executionStats.setPeakMemoryUsage(_resourceMonitor->peak());
      executionStats.setExecutionTime(elapsedSince(_startTime));
      executionStats.setIntermediateCommits(
          _trx->state()->numIntermediateCommits());
    });

    _shutdownState.store(ShutdownState::Done);

    unregisterQueryInTransactionState();

    LOG_TOPIC("5fde0", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " ClusterQuery::finalizeClusterQuery: done"
        << " this: " << (uintptr_t)this;

    return res;
  });
}
