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

#include "Query.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlCallList.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlTransaction.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/GraphNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Optimizer.h"
#include "Aql/Parser.h"
#include "Aql/ProfileLevel.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryExecutionState.h"
#include "Aql/QueryInfoLoggerFeature.h"
#include "Aql/QueryList.h"
#include "Aql/QueryPlanCache.h"
#include "Aql/QueryProfile.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Aql/Timing.h"
#include "Async/async.h"
#include "Auth/Common.h"
#include "Basics/Exceptions.h"
#include "Basics/ResourceUsage.h"
#include "Basics/ScopeGuard.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/fasthash.h"
#include "Basics/system-functions.h"
#include "Cluster/ServerState.h"
#include "Graph/Graph.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Random/RandomGenerator.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "Utils/Events.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#ifdef USE_V8
#include "Aql/QueryResultV8.h"
#include "Transaction/V8Context.h"
#include "V8/JavaScriptSecurityContext.h"
#include "V8/v8-vpack.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/V8Executor.h"
#endif

#include <absl/strings/str_cat.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/Sink.h>

#include <optional>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace {
AqlCallStack const defaultStack{AqlCallList{AqlCall{}}};

constexpr std::string_view fullcountTrue("fullcount:true");
constexpr std::string_view fullcountFalse("fullcount:false");
constexpr std::string_view countTrue("count:true");
constexpr std::string_view countFalse("count:false");
}  // namespace

/// @brief internal constructor, Used to construct a full query or a
/// ClusterQuery
Query::Query(QueryId id, std::shared_ptr<transaction::Context> ctx,
             QueryString queryString,
             std::shared_ptr<VPackBuilder> bindParameters, QueryOptions options,
             std::shared_ptr<SharedQueryState> sharedState)
    : QueryContext(ctx->vocbase(), ctx->operationOrigin(), id),
      _itemBlockManager(*_resourceMonitor),
      _queryString(std::move(queryString)),
      _transactionContext(std::move(ctx)),
      _sharedState(std::move(sharedState)),
#ifdef USE_V8
      _v8Executor(nullptr),
#endif
      _bindParameters(*_resourceMonitor, bindParameters),
      _queryOptions(std::move(options)),
      _trx(nullptr),
      _startTime(currentSteadyClockValue()),
      _endTime(0.0),
      _resultMemoryUsage(0),
      _planMemoryUsage(0),
      _queryHash(kDontCache),
      _shutdownState(ShutdownState::None),
      _executionPhase(ExecutionPhase::INITIALIZE),
      _result(std::nullopt),
#ifdef USE_V8
      _executorOwnedByExterior(_transactionContext->isV8Context() &&
                               v8::Isolate::TryGetCurrent() != nullptr),
      _embeddedQuery(_transactionContext->isV8Context() &&
                     transaction::V8Context::isEmbedded()),
      _registeredInV8Executor(false),
#endif
      _queryHashCalculated(false),
      _registeredQueryInTrx(false),
      _allowDirtyReads(false),
      _queryKilled(false) {
  if (!_transactionContext) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "failed to create query transaction context");
  }

#ifdef USE_V8
  if (_executorOwnedByExterior) {
    // copy transaction options from global state into our local query options
    auto state = transaction::V8Context::getParentState();
    if (state != nullptr) {
      _queryOptions.transactionOptions = state->options();
    }
  }
#endif

  ProfileLevel level = _queryOptions.profile;
  if (level >= ProfileLevel::TraceOne) {
    LOG_TOPIC("22a70", INFO, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " Query::Query queryString: " << _queryString
        << " this: " << (uintptr_t)this;
  } else {
    LOG_TOPIC("11160", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " Query::Query queryString: " << _queryString
        << " this: " << (uintptr_t)this;
  }

  if (bindParameters != nullptr && !bindParameters->isEmpty() &&
      !bindParameters->slice().isNone()) {
    if (level >= ProfileLevel::TraceOne) {
      LOG_TOPIC("8c9fc", INFO, Logger::QUERIES)
          << "bindParameters: " << bindParameters->slice().toJson();
    } else {
      LOG_TOPIC("16a3f", DEBUG, Logger::QUERIES)
          << "bindParameters: " << bindParameters->slice().toJson();
    }
  }

  if (level >= ProfileLevel::TraceOne) {
    VPackBuilder b;
    _queryOptions.toVelocyPack(b, /*disableOptimizerRules*/ false);
    LOG_TOPIC("8979d", INFO, Logger::QUERIES) << "options: " << b.toJson();
  }

  // set memory limit for query
  _resourceMonitor->memoryLimit(_queryOptions.memoryLimit);
  _warnings.updateFromOptions(_queryOptions);

  // store name of user that started the query
  _user = ExecContext::current().user();
}

/// Used to construct a full query. the constructor is protected to ensure
/// that call sites only create Query objects using the `create` factory
/// method
Query::Query(std::shared_ptr<transaction::Context> ctx, QueryString queryString,
             std::shared_ptr<VPackBuilder> bindParameters, QueryOptions options,
             Scheduler* scheduler)
    : Query(0, ctx, std::move(queryString), std::move(bindParameters),
            std::move(options),
            std::make_shared<SharedQueryState>(ctx->vocbase().server(),
                                               scheduler)) {}

Query::~Query() {
  if (!_planSliceCopy.isNone()) {
    _resourceMonitor->decreaseMemoryUsage(_planSliceCopy.byteSize());
  }

  // In the most derived class needs to explicitly call 'destroy()'
  // because otherwise we have potential data races on the vptr
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_wasDestroyed);
#endif
}

void Query::destroy() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(!std::exchange(_wasDestroyed, true));
#endif

  unregisterQueryInTransactionState();
  TRI_ASSERT(!_registeredQueryInTrx);

  _resourceMonitor->decreaseMemoryUsage(_resultMemoryUsage);
  _resultMemoryUsage = 0;

  _resourceMonitor->decreaseMemoryUsage(_planMemoryUsage);
  _planMemoryUsage = 0;

  if (_queryOptions.profile >= ProfileLevel::TraceOne) {
    LOG_TOPIC("36a75", INFO, Logger::QUERIES)
        << elapsedSince(_startTime) << " Query::~Query queryString: "
        << " this: " << (uintptr_t)this;
  }

  if (killed()) {
    setResult({TRI_ERROR_QUERY_KILLED});
  } else {
    // we intentionally set an error here so that we don't accidentially
    // commit the query (again) when calling cleanupPlanAndEngine below.
    setResult({TRI_ERROR_INTERNAL});
  }

  // this will reset _trx, so _trx is invalid after here
  try {
    auto state = cleanupPlanAndEngine(/*sync*/ true);
    TRI_ASSERT(state != ExecutionState::WAITING);
  } catch (...) {
    // unfortunately we cannot do anything here, as we are in the destructor
  }

  _queryProfile.reset();

  unregisterSnippets();

  exitV8Executor();

  _snippets.clear();  // simon: must be before plan
  _plans.clear();     // simon: must be before AST
  _ast.reset();

  LOG_TOPIC("f5cee", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::~Query this: " << (uintptr_t)this;
}

/// @brief factory function for creating a query. this must be used to
/// ensure that Query objects are always created using shared_ptrs.
std::shared_ptr<Query> Query::create(
    std::shared_ptr<transaction::Context> ctx, QueryString queryString,
    std::shared_ptr<velocypack::Builder> bindParameters, QueryOptions options,
    Scheduler* scheduler) {
  TRI_ASSERT(ctx != nullptr);
  // workaround to enable make_shared on a class with a protected constructor
  struct MakeSharedQuery final : Query {
    MakeSharedQuery(std::shared_ptr<transaction::Context> ctx,
                    QueryString queryString,
                    std::shared_ptr<velocypack::Builder> bindParameters,
                    QueryOptions options, Scheduler* scheduler)
        : Query{std::move(ctx), std::move(queryString),
                std::move(bindParameters), std::move(options), scheduler} {}

    ~MakeSharedQuery() final {
      // Destroy this query, otherwise it's still
      // accessible while the query is being destructed,
      // which can result in a data race on the vptr
      destroy();
    }
  };
  TRI_ASSERT(ctx != nullptr);
  return std::make_shared<MakeSharedQuery>(
      std::move(ctx), std::move(queryString), std::move(bindParameters),
      std::move(options), scheduler);
}

/// @brief return the user that started the query
std::string const& Query::user() const { return _user; }

double Query::getLockTimeout() const noexcept {
  return _queryOptions.transactionOptions.lockTimeout;
}

void Query::setLockTimeout(double timeout) noexcept {
  _queryOptions.transactionOptions.lockTimeout = timeout;
}

bool Query::killed() const {
  return (std::numeric_limits<double>::epsilon() < _queryOptions.maxRuntime &&
          _queryOptions.maxRuntime < elapsedSince(_startTime)) ||
         _queryKilled.load(std::memory_order_acquire);
}

/// @brief set the query to killed
void Query::kill() {
  auto const wasKilled = _queryKilled.exchange(true, std::memory_order_acq_rel);
  if (ServerState::instance()->isCoordinator() && !wasKilled) {
    setResult({TRI_ERROR_QUERY_KILLED});
    cleanupPlanAndEngine(/*sync*/ false);
  }
}

void Query::setKillFlag() {
  _queryKilled.store(true, std::memory_order_release);
}

/// @brief the query's transaction id. returns 0 if no transaction
/// has been assigned to the query yet. use this only for informational
/// purposes
TransactionId Query::transactionId() const noexcept {
  if (_trx == nullptr) {
    // no transaction yet. simply return 0
    return TransactionId{0};
  }
  return _trx->tid();
}

/// @brief return the start time of the query (steady clock value)
double Query::startTime() const noexcept { return _startTime; }

double Query::executionTime() const noexcept {
  // should only be called once _endTime has been set
  TRI_ASSERT(_endTime > 0.0);
  return _endTime - _startTime;
}

void Query::ensureExecutionTime() noexcept {
  if (_endTime == 0.0) {
    _endTime = currentSteadyClockValue();
    TRI_ASSERT(_endTime > 0.0);
  }
}

bool Query::tryLoadPlanFromCache() {
  if (_queryOptions.usePlanCache) {
    if (!canUsePlanCache()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_NOT_ELIGIBLE_FOR_PLAN_CACHING);
    }
    // construct plan cache key
    TRI_ASSERT(!_planCacheKey.has_value());
    _planCacheKey.emplace(_vocbase.queryPlanCache().createCacheKey(
        _queryString, bindParametersAsBuilder(), _queryOptions));

    // look up query in query plan cache
    auto cacheEntry = _vocbase.queryPlanCache().lookup(*_planCacheKey);
    if (cacheEntry != nullptr) {
      // entry found in plan cache

      auto hasPermissions = std::invoke([&] {
        // check if the current user has permissions on all the collections
        ExecContext const& exec = ExecContext::current();

        if (!exec.isSuperuser()) {
          for (auto const& dataSource : cacheEntry->dataSources) {
            if (!exec.canUseCollection(dataSource.second.name,
                                       dataSource.second.level)) {
              // cannot use query cache result because of permissions
              return false;
            }
          }
        }

        return true;
      });

      if (!hasPermissions) {
        return false;
      }

      _resourceMonitor->increaseMemoryUsage(
          cacheEntry->serializedPlan.byteSize());
      _planSliceCopy = cacheEntry->serializedPlan;

      return true;
    }
  }
  return false;
}

async<void> Query::prepareQuery() {
  try {
    if (tryLoadPlanFromCache()) {
      auto const querySlice = _planSliceCopy.slice();
      auto const collections = querySlice.get("collections");
      auto const variables = querySlice.get("variables");
      auto const views = querySlice.get("views");
      auto const snippets = querySlice.get("nodes");
      prepareFromVelocyPackWithoutInstantiate(querySlice, collections, views,
                                              variables, snippets);
      co_await instantiatePlan(snippets);

      TRI_ASSERT(!_plans.empty());

      auto& plan = _plans.front();
      plan->findVarUsage();
      plan->findCollectionAccessVariables();
      plan->prepareTraversalOptions();
      enterState(QueryExecutionState::ValueType::EXECUTION);
      co_return;
    }

    std::unique_ptr<ExecutionPlan> plan;
    init(/*createProfile*/ true);

    enterState(QueryExecutionState::ValueType::PARSING);

    plan = preparePlan();

    TRI_ASSERT(plan != nullptr);

    TRI_ASSERT(_trx != nullptr);
    TRI_ASSERT(_trx->status() == transaction::Status::RUNNING);

    // keep serialized copy of unchanged plan to include in query profile
    // necessary because instantiate / execution replace vars and blocks
    bool const keepPlan =
        _queryOptions.profile >= ProfileLevel::Blocks &&
        ServerState::isSingleServerOrCoordinator(_trx->state()->serverRole());
    if (keepPlan) {
      unsigned flags = ExecutionPlan::buildSerializationFlags(
          /*verbose*/ false, /*includeInternals*/ false,
          /*explainRegisters*/ false);
      try {
        auto b = velocypack::Builder();
        plan->toVelocyPack(
            b, flags,
            buildSerializeQueryDataCallback({.includeNumericIds = false,
                                             .includeViews = true,
                                             .includeViewsSeparately = false}));
        _planSliceCopy = std::move(b).sharedSlice();

        TRI_IF_FAILURE("Query::serializePlans1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
        _resourceMonitor->increaseMemoryUsage(_planSliceCopy.byteSize());
      } catch (std::exception const& ex) {
        // must clear _planSliceCopy here so that the destructor of
        // Query doesn't subtract the memory used by _planSliceCopy
        // without us having it tracked properly here.
        _planSliceCopy = {};
        LOG_TOPIC("006c7", ERR, Logger::QUERIES)
            << "unable to convert execution plan to vpack: " << ex.what();
        throw;
      }

      TRI_IF_FAILURE("Query::serializePlans2") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }

    enterState(QueryExecutionState::ValueType::PHYSICAL_INSTANTIATION);

    // simon: assumption is _queryString is empty for DBServer snippets
    bool const planRegisters = !_queryString.empty();
    co_await ExecutionEngine::instantiateFromPlan(*this, *plan, planRegisters);

    _plans.push_back(std::move(plan));

    if (_snippets.size() > 1) {  // register coordinator snippets
      TRI_ASSERT(_trx->state()->isCoordinator());
      QueryRegistry* registry = QueryRegistryFeature::registry();
      if (!registry) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SHUTTING_DOWN,
                                       "query registry not available");
      }
      registry->registerSnippets(_snippets);
    }

    if (_queryProfile) {
      _queryProfile->registerInQueryList();
    }
    registerQueryInTransactionState();

    enterState(QueryExecutionState::ValueType::EXECUTION);
  } catch (Exception const& ex) {
    setResult({ex.code(), ex.what()});
    throw;
  } catch (std::bad_alloc const&) {
    setResult({TRI_ERROR_OUT_OF_MEMORY});
    throw;
  } catch (std::exception const& ex) {
    setResult({TRI_ERROR_INTERNAL, ex.what()});
    throw;
  }
}

void Query::storePlanInCache(ExecutionPlan& plan) {
  plan.planRegisters(ExplainRegisterPlan::No);

  TRI_ASSERT(_planCacheKey.has_value());
  // TODO: verify these options
  unsigned flags = ExecutionPlan::buildSerializationFlags(
      /*verbosePlans*/ true, /*explainInternals*/ true,
      /*explainRegisters*/ false);

  velocypack::Builder serialized;
  // Note that in this serialization it is crucial to include the numeric
  // ids, otherwise the Plan can not be instantiated correctly, when this
  // plan comes back from the query plan cache!
  plan.toVelocyPack(
      serialized, flags,
      buildSerializeQueryDataCallback({.includeNumericIds = true,
                                       .includeViews = false,
                                       .includeViewsSeparately = true}));

  auto dataSources = std::invoke([&]() {
    std::unordered_map<std::string, QueryPlanCache::DataSourceEntry>
        dataSources;
    // add views
    for (auto const& it : _queryDataSources) {
      dataSources.try_emplace(it.first, QueryPlanCache::DataSourceEntry{
                                            it.second, auth::Level::RO});
    }
    // collect transaction DataSources
    _trx->state()->allCollections(
        [&dataSources](TransactionCollection& trxCollection) -> bool {
          auto const& c = trxCollection.collection();
          auth::Level level =
              trxCollection.accessType() == AccessMode::Type::READ
                  ? auth::Level::RO
                  : auth::Level::RW;
          dataSources.try_emplace(
              c->guid(), QueryPlanCache::DataSourceEntry{c->name(), level});
          return true;  // continue traversal
        });
    return dataSources;
  });

  // store plan in query plan cache for future queries
  std::ignore = _vocbase.queryPlanCache().store(
      std::move(*_planCacheKey), std::move(dataSources),
      std::move(serialized).sharedSlice());
  _planCacheKey.reset();
}

/// @brief prepare an AQL query, this is a preparation for execute, but
/// execute calls it internally. The purpose of this separate method is
/// to be able to only prepare a query from VelocyPack and then store it in the
/// QueryRegistry.
std::unique_ptr<ExecutionPlan> Query::preparePlan() {
  TRI_ASSERT(!_queryString.empty());
  LOG_TOPIC("9625e", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::prepare"
      << " this: " << (uintptr_t)this;

  TRI_ASSERT(_ast != nullptr);
  Parser parser(*this, *_ast, _queryString);
  parser.parse();

  // any usage of one of the following features make the query ineligible
  // for query plan caching (at least for now):
  if (_queryOptions.optimizePlanForCaching &&
      (_ast->containsUpsertNode() ||
       _ast->containsAttributeNameValueBindParameters() ||
       _ast->containsCollectionNameValueBindParameters() ||
       _ast->containsGraphNameValueBindParameters() ||
       _ast->containsTraversalDepthValueBindParameters() ||
       _ast->containsUpsertLookupValueBindParameters() || !_warnings.empty())) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_NOT_ELIGIBLE_FOR_PLAN_CACHING);
  }

  // put in collection and attribute name bind parameters (e.g. @@collection or
  // doc.@attr).
  _ast->injectBindParametersFirstStage(_bindParameters, this->resolver());

  _ast->injectBindParametersSecondStage(_bindParameters);

  if (_ast->containsUpsertNode()) {
    // UPSERTs and intermediate commits do not play nice together, because the
    // intermediate commit invalidates the read-own-write iterator required by
    // the subquery. Setting intermediateCommitSize and intermediateCommitCount
    // to UINT64_MAX allows us to effectively disable intermediate commits.
    _queryOptions.transactionOptions.intermediateCommitSize = UINT64_MAX;
    _queryOptions.transactionOptions.intermediateCommitCount = UINT64_MAX;
  }

  TRI_ASSERT(_trx == nullptr);
  // needs to be created after the AST collected all collections
  std::unordered_set<std::string> inaccessibleCollections;
#ifdef USE_ENTERPRISE
  if (_queryOptions.transactionOptions.skipInaccessibleCollections) {
    inaccessibleCollections = _queryOptions.inaccessibleCollections;
  }
#endif

  // create the transaction object
  _trx = AqlTransaction::create(_transactionContext, _collections,
                                _queryOptions.transactionOptions,
                                std::move(inaccessibleCollections));

  _trx->addHint(
      transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel

  // We need to preserve the information about dirty reads, since the
  // transaction who knows might be gone before we have produced the
  // result:
  _allowDirtyReads = _trx->state()->options().allowDirtyReads;

  // As soon as we start to instantiate the plan we have to clean it
  // up before killing the unique_ptr

  // we have an AST, optimize the ast
  enterState(QueryExecutionState::ValueType::AST_OPTIMIZATION);

  _ast->validateAndOptimize(*_trx, {.optimizeNonCacheable = true});

  enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);

  Result res = _trx->begin();

  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }
  TRI_ASSERT(_trx->status() == transaction::Status::RUNNING);

  enterState(QueryExecutionState::ValueType::PLAN_INSTANTIATION);

  auto plan = ExecutionPlan::instantiateFromAst(_ast.get(), true);

  TRI_ASSERT(plan != nullptr);
  injectVertexCollectionIntoGraphNodes(*plan);

  // Run the query optimizer:
  enterState(QueryExecutionState::ValueType::PLAN_OPTIMIZATION);
  Optimizer opt(*_resourceMonitor, _queryOptions.maxNumberOfPlans);
  // get enabled/disabled rules
  opt.createPlans(std::move(plan), _queryOptions, false);
  // Now plan and all derived plans belong to the optimizer
  plan = opt.stealBest();  // Now we own the best one again

  TRI_ASSERT(plan != nullptr);

  // return the V8 executor if we are in one
  exitV8Executor();

  // validate that all bind parameters are in use
  _bindParameters.validateAllUsed();

  plan->findVarUsage();

  if (_planCacheKey.has_value()) {
    TRI_ASSERT(_queryOptions.optimizePlanForCaching &&
               _queryOptions.usePlanCache);

    // if query parsing/optimization produces warnings. we must disable query
    // plan caching.
    // additionally, if the query contains a REMOTE_SINGLE/REMOTE_MULTIPLE node,
    // we must also disable caching, because these node types do not have
    // constructors for being created from serialized velocypack input
    bool canCachePlan = _warnings.empty() &&
                        !plan->contains(ExecutionNode::REMOTE_SINGLE) &&
                        !plan->contains(ExecutionNode::REMOTE_MULTIPLE);

    if (canCachePlan) {
      // store result plan in query plan cache
      storePlanInCache(*plan);
    } else {
      // do not store plan in cache
      _planCacheKey.reset();
    }
  }

  return plan;
}

/// @brief execute an AQL query
ExecutionState Query::execute(QueryResult& queryResult) {
  LOG_TOPIC("e8ed7", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::execute"
      << " this: " << (uintptr_t)this;

  try {
    if (killed()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    }

    bool useQueryCache = canUseResultsCache();
    switch (_executionPhase) {
      case ExecutionPhase::INITIALIZE: {
        if (useQueryCache) {
          // check the query cache for an existing result
          auto cacheEntry = QueryCache::instance()->lookup(
              &_vocbase, hash(), _queryString, bindParametersAsBuilder());

          if (cacheEntry != nullptr) {
            if (cacheEntry->currentUserHasPermissions()) {
              // we don't have yet a transaction when we're here, so let's
              // create a mimimal context to build the result
              queryResult.context = transaction::StandaloneContext::create(
                  _vocbase, operationOrigin());
              TRI_ASSERT(cacheEntry->_queryResult != nullptr);
              queryResult.data = cacheEntry->_queryResult;
              queryResult.extra = cacheEntry->_stats;
              queryResult.cached = true;
              // Note: cached queries were never done with dirty reads,
              // so we can always hand out the result here without extra
              // HTTP header.
              return ExecutionState::DONE;
            }
            // if no permissions, fall through to regular querying
          }
        }

        TRI_ASSERT(!_prepareResult.valid());
        // will throw if it fails
        auto prepare = [this]() -> futures::Future<futures::Unit> {
          if (!_ast) {  // simon: hack for AQL_EXECUTEJSON
            co_return co_await prepareQuery();
          }
          co_return;
        }();
        _executionPhase = ExecutionPhase::PREPARE;
        if (!prepare.isReady()) {
          std::move(prepare).thenFinal(
              [this, sqs = sharedState()](auto&& tryResult) {
                sqs->executeAndWakeup([this, tryResult = std::move(tryResult)] {
                  _prepareResult = std::move(tryResult);
                  TRI_ASSERT(_prepareResult.valid());
                  return true;
                });
              });
          return ExecutionState::WAITING;
        } else {
          _prepareResult = std::move(prepare).result();
          TRI_ASSERT(_prepareResult.valid());
        }
      }
        [[fallthrough]];
      case ExecutionPhase::PREPARE: {
        TRI_ASSERT(_prepareResult.valid());
        _prepareResult.throwIfFailed();

        logAtStart();
        if (_planCacheKey.has_value()) {
          queryResult.planCacheKey = _planCacheKey->hash();
        }
        // NOTE: If the options have a shorter lifetime than the builder, it
        // gets invalid (at least set() and close() are broken).
        queryResult.data = std::make_shared<VPackBuilder>(&vpackOptions());

        // reserve some space in Builder to avoid frequent reallocs
        queryResult.data->reserve(16 * 1024);
        queryResult.data->openArray(/*unindexed*/ true);

        _executionPhase = ExecutionPhase::EXECUTE;
      }
        [[fallthrough]];
      case ExecutionPhase::EXECUTE: {
        TRI_ASSERT(queryResult.data != nullptr);
        TRI_ASSERT(queryResult.data->isOpenArray());
        TRI_ASSERT(_trx != nullptr) << "id=" << id();

        if (useQueryCache && (isModificationQuery() || !_warnings.empty() ||
                              !_ast->root()->isCacheable())) {
          useQueryCache = false;
        }

        ExecutionEngine* engine = this->rootEngine();
        TRI_ASSERT(engine != nullptr);

        // this is the RegisterId our results can be found in
        RegisterId const resultRegister = engine->resultRegister();

        // We loop as long as we are having ExecState::DONE returned
        // In case of WAITING we return, this function is repeatable!
        // In case of HASMORE we loop
        while (true) {
          auto const& [state, skipped, block] = engine->execute(::defaultStack);
          // The default call asks for No skips.
          TRI_ASSERT(skipped.nothingSkipped());
          if (state == ExecutionState::WAITING) {
            return state;
          }

          // block == nullptr => state == DONE
          if (block == nullptr) {
            TRI_ASSERT(state == ExecutionState::DONE);
            break;
          }

          if (!_queryOptions.silent && resultRegister.isValid()) {
            // cache low-level pointer to avoid repeated shared-ptr-derefs
            TRI_ASSERT(queryResult.data != nullptr);
            auto& resultBuilder = *queryResult.data;
            size_t previousLength = resultBuilder.bufferRef().byteSize();
            auto& vpackOpts = vpackOptions();

            size_t const n = block->numRows();

            for (size_t i = 0; i < n; ++i) {
              AqlValue const& val = block->getValueReference(i, resultRegister);

              if (!val.isEmpty()) {
                val.toVelocyPack(&vpackOpts, resultBuilder,
                                 /*allowUnindexed*/ true);
              }
            }

            size_t newLength = resultBuilder.bufferRef().byteSize();
            TRI_ASSERT(newLength >= previousLength);
            size_t diff = newLength - previousLength;

            _resourceMonitor->increaseMemoryUsage(diff);
            _resultMemoryUsage += diff;
          }

          if (state == ExecutionState::DONE) {
            break;
          }
        }

        // must close result array here because it must be passed as a closed
        // array to the query cache
        queryResult.data->close();
        queryResult.allowDirtyReads = _allowDirtyReads;
        if (_planCacheKey.has_value()) {
          queryResult.planCacheKey = _planCacheKey->hash();
        }

        if (useQueryCache && !_allowDirtyReads && _warnings.empty()) {
          // Cannot cache dirty reads! Yes, the query cache is not used in
          // the cluster anyway, but we leave this condition in here for
          // a future in which the query cache could be used in the cluster!
          std::unordered_map<std::string, std::string> dataSources =
              _queryDataSources;

          _trx->state()->allCollections(  // collect transaction DataSources
              [&dataSources](TransactionCollection& trxCollection) -> bool {
                auto const& c = trxCollection.collection();
                dataSources.try_emplace(c->guid(), c->name());
                return true;  // continue traversal
              });

          // create a query cache entry for later storage
          _cacheEntry = std::make_unique<QueryCacheResultEntry>(
              hash(), _queryString, queryResult.data, bindParametersAsBuilder(),
              std::move(dataSources)  // query DataSources
          );
        }

        queryResult.context = _trx->transactionContext();
        _executionPhase = ExecutionPhase::FINALIZE;
      }

        [[fallthrough]];
      case ExecutionPhase::FINALIZE: {
        if (!queryResult.extra) {
          queryResult.extra = std::make_shared<VPackBuilder>();
        }
        // will set warnings, stats, profile and cleanup plan and engine
        auto state = finalize(*queryResult.extra);
        bool isCachingAllowed = !_transactionContext->isStreaming() ||
                                _trx->state()->isReadOnlyTransaction();
        if (state == ExecutionState::DONE && _cacheEntry != nullptr &&
            isCachingAllowed) {
          _cacheEntry->_stats = queryResult.extra;
          QueryCache::instance()->store(&_vocbase, std::move(_cacheEntry));
        }

        return state;
      }
    }
    // We should not be able to get here
    TRI_ASSERT(false);
  } catch (Exception const& ex) {
    setResult({ex.code(), absl::StrCat("AQL: ", ex.message(),
                                       QueryExecutionState::toStringWithPrefix(
                                           _execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  } catch (std::bad_alloc const&) {
    setResult(
        {TRI_ERROR_OUT_OF_MEMORY,
         absl::StrCat(TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                      QueryExecutionState::toStringWithPrefix(_execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  } catch (std::exception const& ex) {
    setResult({TRI_ERROR_INTERNAL,
               absl::StrCat(ex.what(), QueryExecutionState::toStringWithPrefix(
                                           _execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  }

  return ExecutionState::DONE;
}

/**
 * @brief Execute the query in a synchronous way, so if
 *        the query needs to wait (e.g. IO) this thread
 *        will be blocked.
 *
 * @param registry The query registry.
 *
 * @return The result of this query. The result is always complete
 */
QueryResult Query::executeSync() {
  _executeCallerWaiting = ExecuteCallerWaiting::Synchronously;
  std::shared_ptr<SharedQueryState> ss;

  QueryResult queryResult;
  do {
    auto state = execute(queryResult);
    if (state != ExecutionState::WAITING) {
      TRI_ASSERT(state == ExecutionState::DONE);
      return queryResult;
    }

    if (!ss) {
      ss = sharedState();
    }

    TRI_ASSERT(ss != nullptr);
    ss->waitForAsyncWakeup();
  } while (true);
}

#ifdef USE_V8
// execute an AQL query: may only be called with an active V8 handle scope
QueryResultV8 Query::executeV8(v8::Isolate* isolate) {
  _executeCallerWaiting = ExecuteCallerWaiting::Synchronously;
  LOG_TOPIC("6cac7", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::executeV8"
      << " this: " << (uintptr_t)this;

  QueryResultV8 queryResult;

  try {
    bool useQueryCache = canUseResultsCache();

    if (useQueryCache) {
      // check the query cache for an existing result
      auto cacheEntry = QueryCache::instance()->lookup(
          &_vocbase, hash(), _queryString, bindParametersAsBuilder());

      if (cacheEntry != nullptr) {
        if (cacheEntry->currentUserHasPermissions()) {
          // we don't have yet a transaction when we're here, so let's create
          // a mimimal context to build the result
          queryResult.context = transaction::StandaloneContext::create(
              _vocbase, operationOrigin());
          v8::Handle<v8::Value> values =
              TRI_VPackToV8(isolate, cacheEntry->_queryResult->slice(),
                            queryResult.context->getVPackOptions());
          TRI_ASSERT(values->IsArray());
          queryResult.v8Data = v8::Handle<v8::Array>::Cast(values);
          queryResult.extra = cacheEntry->_stats;
          queryResult.cached = true;
          return queryResult;
        }
        // if no permissions, fall through to regular querying
      }
    }

    // will throw if it fails
    [&]() -> futures::Future<futures::Unit> {
      co_return co_await prepareQuery();
    }()
                 .waitAndGet();

    logAtStart();

    if (useQueryCache && (isModificationQuery() || !_warnings.empty() ||
                          !_ast->root()->isCacheable())) {
      useQueryCache = false;
    }

    v8::Handle<v8::Array> resArray = v8::Array::New(isolate);

    auto* engine = this->rootEngine();
    TRI_ASSERT(engine != nullptr);

    std::shared_ptr<SharedQueryState> ss = engine->sharedState();
    TRI_ASSERT(ss != nullptr);

    // this is the RegisterId our results can be found in
    auto const resultRegister = engine->resultRegister();

    // following options and builder only required for query cache
    VPackOptions options = VPackOptions::Defaults;
    options.buildUnindexedArrays = true;
    options.buildUnindexedObjects = true;
    auto builder = std::make_shared<VPackBuilder>(&options);

    try {
      ss->resetWakeupHandler();

      // iterate over result, return it and optionally store it in query cache
      builder->openArray();

      // iterate over result and return it
      auto context = TRI_IGETC;
      uint32_t j = 0;
      ExecutionState state = ExecutionState::HASMORE;
      SkipResult skipped;
      SharedAqlItemBlockPtr value;
      while (state != ExecutionState::DONE) {
        std::tie(state, skipped, value) = engine->execute(::defaultStack);
        // We cannot trigger a skip operation from here
        TRI_ASSERT(skipped.nothingSkipped());

        while (state == ExecutionState::WAITING) {
          ss->waitForAsyncWakeup();
          std::tie(state, skipped, value) = engine->execute(::defaultStack);
          TRI_ASSERT(skipped.nothingSkipped());
        }

        // value == nullptr => state == DONE
        TRI_ASSERT(value != nullptr || state == ExecutionState::DONE);

        if (value == nullptr) {
          continue;
        }

        if (!_queryOptions.silent && resultRegister.isValid()) {
          TRI_IF_FAILURE(
              "Query::executeV8directKillBeforeQueryResultIsGettingHandled") {
            debugKillQuery();
          }
          size_t memoryUsage = 0;
          size_t const n = value->numRows();

          auto const& vpackOpts = vpackOptions();
          for (size_t i = 0; i < n; ++i) {
            AqlValue const& val = value->getValueReference(i, resultRegister);

            if (!val.isEmpty()) {
              resArray->Set(context, j++, val.toV8(isolate, &vpackOptions()))
                  .FromMaybe(false);

              if (useQueryCache) {
                val.toVelocyPack(&vpackOpts, *builder, /*allowUnindexed*/ true);
              }
              memoryUsage += sizeof(v8::Value);
              if (val.requiresDestruction()) {
                memoryUsage += val.memoryUsage();
              }

              if (V8PlatformFeature::isOutOfMemory(isolate)) {
                THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
              }
            }
          }

          // this may throw
          _resourceMonitor->increaseMemoryUsage(memoryUsage);
          _resultMemoryUsage += memoryUsage;

          TRI_IF_FAILURE(
              "Query::executeV8directKillAfterQueryResultIsGettingHandled") {
            debugKillQuery();
          }
        }
      }

      builder->close();
    } catch (...) {
      LOG_TOPIC("8a6bf", DEBUG, Logger::QUERIES)
          << elapsedSince(_startTime) << " got an exception executing "
          << " this: " << (uintptr_t)this;
      throw;
    }

    if (_planCacheKey.has_value()) {
      queryResult.planCacheKey = _planCacheKey->hash();
    }
    queryResult.v8Data = resArray;
    queryResult.context = _trx->transactionContext();
    queryResult.extra = std::make_shared<VPackBuilder>();
    queryResult.allowDirtyReads = _allowDirtyReads;

    if (useQueryCache && _warnings.empty()) {
      auto dataSources = _queryDataSources;
      _trx->state()->allCollections(  // collect transaction DataSources
          [&dataSources](TransactionCollection& trxCollection) -> bool {
            auto const& c = trxCollection.collection();
            dataSources.try_emplace(c->guid(), c->name());
            return true;  // continue traversal
          });

      // create a cache entry for later usage
      _cacheEntry = std::make_unique<QueryCacheResultEntry>(
          hash(), _queryString, builder, bindParametersAsBuilder(),
          std::move(dataSources)  // query DataSources
      );
    }

    ss->resetWakeupHandler();

    // will set warnings, stats, profile and cleanup plan and engine
    ExecutionState state = finalize(*queryResult.extra);
    while (state == ExecutionState::WAITING) {
      ss->waitForAsyncWakeup();
      state = finalize(*queryResult.extra);
    }
    bool isCachingAllowed = !_transactionContext->isTransactionJS() ||
                            _trx->state()->isReadOnlyTransaction();

    if (_cacheEntry != nullptr && isCachingAllowed) {
      _cacheEntry->_stats = queryResult.extra;
      QueryCache::instance()->store(&_vocbase, std::move(_cacheEntry));
    }

    // fallthrough to returning queryResult below...
  } catch (Exception const& ex) {
    setResult({ex.code(), absl::StrCat("AQL: ", ex.message(),
                                       QueryExecutionState::toStringWithPrefix(
                                           _execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  } catch (std::bad_alloc const&) {
    setResult(
        {TRI_ERROR_OUT_OF_MEMORY,
         absl::StrCat(TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                      QueryExecutionState::toStringWithPrefix(_execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  } catch (std::exception const& ex) {
    setResult({TRI_ERROR_INTERNAL,
               absl::StrCat(ex.what(), QueryExecutionState::toStringWithPrefix(
                                           _execState))});
    cleanupPlanAndEngine(/*sync*/ true);
    queryResult.reset(result());
  }

  return queryResult;
}
#endif

ExecutionState Query::finalize(velocypack::Builder& extras) {
  if (_queryProfile != nullptr &&
      _shutdownState.load(std::memory_order_relaxed) == ShutdownState::None) {
    // the following call removes the query from the list of currently
    // running queries. so whoever fetches that list will not see a Query that
    // is about to shut down/be destroyed
    _queryProfile->unregisterFromQueryList();
  }

  auto state = cleanupPlanAndEngine(/*sync*/ false);
  if (state == ExecutionState::WAITING) {
    return state;
  }

  extras.openObject(/*unindexed*/ true);
  _warnings.toVelocyPack(extras);

  if (!_snippets.empty()) {
    executionStatsGuard().doUnderLock([&](auto& executionStats) {
      executionStats.requests += _numRequests.load(std::memory_order_relaxed);
      executionStats.setPeakMemoryUsage(_resourceMonitor->peak());
      executionStats.setExecutionTime(executionTime());
      executionStats.setIntermediateCommits(
          _trx->state()->numIntermediateCommits());
      for (auto& engine : _snippets) {
        engine->collectExecutionStats(executionStats);
      }

      // execution statistics
      extras.add(VPackValue("stats"));
      executionStats.toVelocyPack(extras, _queryOptions.fullCount);
    });

    bool keepPlan =
        !_planSliceCopy.isNone() &&
        _queryOptions.profile >= ProfileLevel::Blocks &&
        ServerState::isSingleServerOrCoordinator(_trx->state()->serverRole());

    if (keepPlan) {
      extras.add("plan", _planSliceCopy.slice());
    }
  }

  double now = currentSteadyClockValue();
  if (_queryProfile != nullptr &&
      _queryOptions.profile >= ProfileLevel::Basic) {
    _queryProfile->setStateDone(QueryExecutionState::ValueType::FINALIZATION);
    _queryProfile->toVelocyPack(extras);
  }
  extras.close();

  setResult({TRI_ERROR_NO_ERROR});

  LOG_TOPIC("95996", DEBUG, Logger::QUERIES)
      << now - _startTime << " Query::finalize:returning"
      << " this: " << (uintptr_t)this;

  return ExecutionState::DONE;
}

/// @brief parse an AQL query
QueryResult Query::parse() {
  // only used in case case of failure
  QueryResult result;

  try {
    init(/*createProfile*/ false);
    Parser parser(*this, *_ast, _queryString);
    return parser.parseWithDetails();

  } catch (Exception const& ex) {
    result.reset(Result(ex.code(), ex.message()));
  } catch (std::bad_alloc const&) {
    result.reset(Result(TRI_ERROR_OUT_OF_MEMORY));
  } catch (std::exception const& ex) {
    result.reset(Result(TRI_ERROR_INTERNAL, ex.what()));
  }

  TRI_ASSERT(result.fail());
  return result;
}

/// @brief explain an AQL query
QueryResult Query::explain() {
  QueryResult result;
  result.extra = std::make_shared<VPackBuilder>();

  VPackOptions options;
  options.checkAttributeUniqueness = false;
  options.buildUnindexedArrays = true;
  result.data = std::make_shared<VPackBuilder>(&options);

  try {
    if (tryLoadPlanFromCache()) {
      TRI_ASSERT(_planCacheKey.has_value());
      TRI_ASSERT(!_planSliceCopy.isNone());

      enterState(QueryExecutionState::ValueType::FINALIZATION);
      result.data->add(_planSliceCopy);
      {
        VPackBuilder& b = *result.extra;
        VPackObjectBuilder guard(&b, /*unindexed*/ true);
        // warnings
        TRI_ASSERT(_warnings.empty());
        _warnings.toVelocyPack(b);
        b.add(VPackValue("stats"));
        {
          // optimizer statistics
          ensureExecutionTime();
          VPackObjectBuilder guard(&b, /*unindexed*/ true);
          Optimizer::Stats::toVelocyPackForCachedPlan(b);
          b.add("peakMemoryUsage", VPackValue(_resourceMonitor->peak()));
          b.add("executionTime", VPackValue(executionTime()));
        }
        result.planCacheKey = _planCacheKey->hash();
      }
      return result;
    }

    init(/*createProfile*/ false);
    enterState(QueryExecutionState::ValueType::PARSING);

    Parser parser(*this, *_ast, _queryString);
    parser.parse();

    // any usage of one of the following features make the query ineligible
    // for query plan caching (at least for now):
    if (_queryOptions.optimizePlanForCaching &&
        (_ast->containsUpsertNode() ||
         _ast->containsAttributeNameValueBindParameters() ||
         _ast->containsCollectionNameValueBindParameters() ||
         _ast->containsGraphNameValueBindParameters() ||
         _ast->containsTraversalDepthValueBindParameters() ||
         _ast->containsUpsertLookupValueBindParameters() ||
         !_warnings.empty())) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_NOT_ELIGIBLE_FOR_PLAN_CACHING);
    }

    // put in bind parameters
    parser.ast()->injectBindParametersFirstStage(_bindParameters,
                                                 this->resolver());
    _ast->injectBindParametersSecondStage(_bindParameters);
    _bindParameters.validateAllUsed();

    // optimize and validate the ast
    enterState(QueryExecutionState::ValueType::AST_OPTIMIZATION);

    // create the transaction object
    _trx = AqlTransaction::create(_transactionContext, _collections,
                                  _queryOptions.transactionOptions);

    enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);

    Result res = _trx->begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    _ast->validateAndOptimize(*_trx, {.optimizeNonCacheable = true});

    enterState(QueryExecutionState::ValueType::PLAN_INSTANTIATION);
    std::unique_ptr<ExecutionPlan> plan =
        ExecutionPlan::instantiateFromAst(parser.ast(), true);

    TRI_ASSERT(plan != nullptr);
    injectVertexCollectionIntoGraphNodes(*plan);

    // Run the query optimizer:
    enterState(QueryExecutionState::ValueType::PLAN_OPTIMIZATION);
    Optimizer opt(*_resourceMonitor, _queryOptions.maxNumberOfPlans);
    // get enabled/disabled rules
    opt.createPlans(std::move(plan), _queryOptions, true);

    enterState(QueryExecutionState::ValueType::FINALIZATION);

    auto preparePlanForSerialization =
        [&](std::unique_ptr<ExecutionPlan> const& plan) {
          plan->findVarUsage();
          plan->planRegisters(_queryOptions.explainRegisters);
          plan->findCollectionAccessVariables();
          plan->prepareTraversalOptions();
        };

    // build serialization flags for execution plan
    unsigned flags = ExecutionPlan::buildSerializationFlags(
        _queryOptions.verbosePlans, _queryOptions.explainInternals,
        _queryOptions.explainRegisters == ExplainRegisterPlan::Yes);

    ResourceUsageScope scope(*_resourceMonitor);

    if (_queryOptions.allPlans) {
      VPackArrayBuilder guard(result.data.get());

      size_t previousSize = result.data->bufferRef().byteSize();
      auto const& plans = opt.getPlans();
      for (auto& it : plans) {
        auto& pln = it.first;
        TRI_ASSERT(pln != nullptr);

        preparePlanForSerialization(pln);

        pln->toVelocyPack(
            *result.data, flags,
            buildSerializeQueryDataCallback({.includeNumericIds = false,
                                             .includeViews = true,
                                             .includeViewsSeparately = false}));
        TRI_IF_FAILURE("Query::serializePlans1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        // memory accounting for different execution plans
        size_t currentSize = result.data->bufferRef().byteSize();
        scope.increase(currentSize - previousSize);
        previousSize = currentSize;

        TRI_IF_FAILURE("Query::serializePlans2") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
      }
      // cachability not available here
      result.cached = false;
    } else {
      std::unique_ptr<ExecutionPlan> bestPlan =
          opt.stealBest();  // Now we own the best one again
      TRI_ASSERT(bestPlan != nullptr);

      preparePlanForSerialization(bestPlan);
      bestPlan->toVelocyPack(
          *result.data, flags,
          buildSerializeQueryDataCallback({.includeNumericIds = false,
                                           .includeViews = true,
                                           .includeViewsSeparately = false}));

      TRI_IF_FAILURE("Query::serializePlans1") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      scope.increase(result.data->bufferRef().byteSize());

      TRI_IF_FAILURE("Query::serializePlans2") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      // cachability
      result.cached = (!_queryString.empty() && !isModificationQuery() &&
                       _warnings.empty() && _ast->root()->isCacheable());

      if (_planCacheKey.has_value()) {
        // if query parsing/optimization produces warnings. we must disable
        // query plan caching. additionally, if the query contains a
        // REMOTE_SINGLE/REMOTE_MULTIPLE node, we must also disable caching,
        // because these node types do not have constructors for being created
        // from serialized velocypack input
        bool canCachePlan = _warnings.empty() &&
                            !bestPlan->contains(ExecutionNode::REMOTE_SINGLE) &&
                            !bestPlan->contains(ExecutionNode::REMOTE_MULTIPLE);

        if (canCachePlan) {
          // store result plan in query plan cache
          storePlanInCache(*bestPlan);
        } else {
          // do not store plan in cache
          _planCacheKey.reset();
        }
      }
    }

    // the query object no owns the memory used by the plan(s)
    _planMemoryUsage += scope.trackedAndSteal();

    // technically no need to commit, as we are only explaining here
    auto commitResult = _trx->commit();
    if (commitResult.fail()) {
      THROW_ARANGO_EXCEPTION(commitResult);
    }

    {
      VPackBuilder& b = *result.extra;
      VPackObjectBuilder guard(&b, /*unindexed*/ true);
      // warnings
      _warnings.toVelocyPack(b);
      b.add(VPackValue("stats"));
      {
        // optimizer statistics
        ensureExecutionTime();
        VPackObjectBuilder guard(&b, /*unindexed*/ true);
        opt.toVelocyPack(b);
        b.add("peakMemoryUsage", VPackValue(_resourceMonitor->peak()));
        b.add("executionTime", VPackValue(executionTime()));
      }
    }
  } catch (Exception const& ex) {
    result.reset(Result(
        ex.code(),
        absl::StrCat(ex.message(),
                     QueryExecutionState::toStringWithPrefix(_execState))));
  } catch (std::bad_alloc const&) {
    result.reset(Result(
        TRI_ERROR_OUT_OF_MEMORY,
        absl::StrCat(TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                     QueryExecutionState::toStringWithPrefix(_execState))));
  } catch (std::exception const& ex) {
    result.reset(Result(
        TRI_ERROR_INTERNAL,
        absl::StrCat(ex.what(),
                     QueryExecutionState::toStringWithPrefix(_execState))));
  }

  // will be returned in success or failure case
  return result;
}

bool Query::isModificationQuery() const noexcept {
  TRI_ASSERT(_ast != nullptr);
  return _ast->containsModificationNode();
}

bool Query::isAsyncQuery() const noexcept {
  TRI_ASSERT(_ast != nullptr);
  return _ast->canApplyParallelism();
}

/// @brief enter a V8 executor
void Query::enterV8Executor() {
#ifdef USE_V8
  auto registerCtx = [&] {
    // register transaction in context
    if (_transactionContext->isV8Context()) {
      auto ctx =
          static_cast<transaction::V8Context*>(_transactionContext.get());
      ctx->enterV8Context();
    } else {
      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      TRI_ASSERT(isolate != nullptr);
      TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(
          isolate->GetData(V8PlatformFeature::V8_DATA_SLOT));
      v8g->_transactionState = _trx->stateShrdPtr();
    }
  };

  if (!_executorOwnedByExterior) {
    if (_v8Executor == nullptr) {
      auto& server = vocbase().server();
      if (!server.hasFeature<V8DealerFeature>() ||
          !server.isEnabled<V8DealerFeature>()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "V8 engine is disabled");
      }
      JavaScriptSecurityContext securityContext =
          JavaScriptSecurityContext::createQueryContext();
      _v8Executor = server.getFeature<V8DealerFeature>().enterExecutor(
          &_vocbase, securityContext);

      if (_v8Executor == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_RESOURCE_LIMIT,
            "unable to enter V8 executor for query execution");
      }
      registerCtx();
    }
    TRI_ASSERT(_v8Executor != nullptr);
  } else if (!_embeddedQuery &&
             !_registeredInV8Executor) {  // may happen for stream trx
    registerCtx();
    _registeredInV8Executor = true;
  }
#endif
}

/// @brief return a V8 executor
void Query::exitV8Executor() {
#ifdef USE_V8
  auto unregister = [&] {
    if (_transactionContext->isV8Context()) {  // necessary for stream trx
      auto ctx =
          static_cast<transaction::V8Context*>(_transactionContext.get());
      ctx->exitV8Context();
    } else {
      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      TRI_ASSERT(isolate != nullptr);
      TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(
          isolate->GetData(V8PlatformFeature::V8_DATA_SLOT));
      v8g->_transactionState = nullptr;
    }
  };
  if (!_executorOwnedByExterior) {
    if (_v8Executor != nullptr) {
      // unregister transaction in context
      unregister();

      auto& server = vocbase().server();
      TRI_ASSERT(server.hasFeature<V8DealerFeature>() &&
                 server.isEnabled<V8DealerFeature>());
      server.getFeature<V8DealerFeature>().exitExecutor(_v8Executor);
      _v8Executor = nullptr;
    }
  } else if (!_embeddedQuery && _registeredInV8Executor) {
    // prevent duplicate deregistration
    unregister();
    _registeredInV8Executor = false;
  }
#endif
}

#ifdef USE_V8
void Query::runInV8ExecutorContext(
    std::function<void(v8::Isolate*)> const& cb) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  TRI_ASSERT(isolate != nullptr);

  if (_executorOwnedByExterior) {
    TRI_ASSERT(_v8Executor == nullptr);
    TRI_ASSERT(isolate->InContext());

    cb(isolate);
  } else {
    TRI_ASSERT(!isolate->InContext());
    TRI_ASSERT(_v8Executor != nullptr);

    _v8Executor->runInContext(
        [&cb](v8::Isolate* isolate) -> Result {
          cb(isolate);
          return {};
        },
        /*executeGlobalMethods*/ false);
  }
}
#endif

/// @brief build traverser engines. only used on DB servers
void buildTraverserEngines(velocypack::Slice /*traverserSlice*/,
                           velocypack::Builder& /*answerBuilder*/) {}

/// @brief initializes the query
void Query::init(bool createProfile) {
  TRI_ASSERT(!_queryProfile && !_ast);
  if (_queryProfile || _ast) {
    // already called
    return;
  }

  TRI_ASSERT(_queryProfile == nullptr);
  // adds query to QueryList which is needed for /_api/query/current
  if (createProfile && !ServerState::instance()->isDBServer()) {
    _queryProfile = std::make_unique<QueryProfile>(*this);
  }
  enterState(QueryExecutionState::ValueType::INITIALIZATION);

  TRI_ASSERT(_ast == nullptr);
  AstPropertiesFlagsType flags = AstPropertyFlag::AST_FLAG_DEFAULT;
  // Create a plan that can be executed with different sets of bind parameters.
  if (_queryOptions.optimizePlanForCaching) {
    flags |= AstPropertyFlag::NON_CONST_PARAMETERS;
  }
  _ast = std::make_unique<Ast>(*this, flags);
}

void Query::registerQueryInTransactionState() {
  TRI_ASSERT(!_registeredQueryInTrx);
  // register ourselves in the TransactionState
  _trx->state()->beginQuery(resourceMonitorAsSharedPtr(),
                            isModificationQuery());
  _registeredQueryInTrx = true;
}

void Query::unregisterQueryInTransactionState() noexcept {
  if (_registeredQueryInTrx) {
    TRI_ASSERT(_trx != nullptr && _trx->state() != nullptr);
    // unregister ourselves in the TransactionState
    _trx->state()->endQuery(isModificationQuery());
    _registeredQueryInTrx = false;
  }
}

/// @brief calculate a hash for the query, once
uint64_t Query::hash() {
  if (!_queryHashCalculated) {
    _queryHash = calculateHash();
    _queryHashCalculated = true;
  }
  return _queryHash;
}

/// @brief whether or not the query plan cache can be used for the query
bool Query::canUsePlanCache() const noexcept {
  if (!_queryOptions.optimizePlanForCaching) {
    return false;
  }
  if (_queryOptions.allPlans) {
    // if we are required to return all plans, we cannot simply retrieve
    // a single plan from the plan cache.
    return false;
  }
  if (_queryOptions.explainRegisters == ExplainRegisterPlan::Yes) {
    // if we are required to return register information, we cannot use a
    // cached plan.
    return false;
  }
  if (!_queryOptions.optimizerRules.empty()) {
    // user-defined setting of optimizer rules. we don't want to
    // distinguish between multiple plans for the same query but with
    // different sets of optimizer rules, so we simply bail out here.
    return false;
  }
#ifdef USE_ENTERPRISE
  // declaring any inaccessible collections in the query options
  // disables plan caching, because otherwise the inaccessible
  // collections would need to become part of the cached plan.
  if (!_queryOptions.inaccessibleCollections.empty()) {
    return false;
  }
#endif
  // restricting the query to certain shards via options disables
  // plan caching.
  if (!_queryOptions.restrictToShards.empty()) {
    return false;
  }

  return true;
}

/// @brief log a query
void Query::logAtStart() const {
  if (_queryString.empty() ||
      !vocbase().server().hasFeature<QueryRegistryFeature>()) {
    return;
  }
  auto const& feature = vocbase().server().getFeature<QueryRegistryFeature>();
  size_t maxLength = feature.maxQueryStringLength();

  LOG_TOPIC("8a86a", TRACE, Logger::QUERIES)
      << "executing query " << _queryId << ": '"
      << _queryString.extract(maxLength) << "'";
}

void Query::logAtEnd() const {
  if (_queryString.empty() ||
      !vocbase().server().hasFeature<QueryRegistryFeature>()) {
    return;
  }

  auto const& feature = vocbase().server().getFeature<QueryRegistryFeature>();

  // log failed queries?
  bool logFailed = feature.logFailedQueries() && result().fail();
  // log queries exceeding memory usage threshold
  bool logMemoryUsage =
      resourceMonitor().peak() >= feature.peakMemoryUsageThreshold();

  if (!logFailed && !logMemoryUsage) {
    return;
  }

  size_t maxLength = feature.maxQueryStringLength();

  std::string bindParameters;
  if (feature.trackBindVars()) {
    // also log bind variables
    stringifyBindParameters(bindParameters, ", bind vars: ", maxLength);
  }

  std::string dataSources;
  if (feature.trackDataSources()) {
    stringifyDataSources(dataSources, ", data sources: ");
  }

  if (logFailed) {
    LOG_TOPIC("d499d", WARN, Logger::QUERIES)
        << "AQL " << (queryOptions().stream ? "streaming " : "") << "query '"
        << extractQueryString(maxLength, feature.trackQueryString()) << "'"
        << bindParameters << dataSources << ", database: " << vocbase().name()
        << ", user: " << user() << ", id: " << _queryId << ", token: QRY"
        << _queryId << ", peak memory usage: " << resourceMonitor().peak()
        << " failed with exit code " << result().errorNumber() << ": "
        << result().errorMessage()
        << ", took: " << Logger::FIXED(executionTime());
  } else {
    LOG_TOPIC("e0b7c", WARN, Logger::QUERIES)
        << "AQL " << (queryOptions().stream ? "streaming " : "") << "query '"
        << extractQueryString(maxLength, feature.trackQueryString()) << "'"
        << bindParameters << dataSources << ", database: " << vocbase().name()
        << ", user: " << user() << ", id: " << _queryId << ", token: QRY"
        << _queryId << ", peak memory usage: " << resourceMonitor().peak()
        << " used more memory than configured memory usage alerting threshold "
        << feature.peakMemoryUsageThreshold()
        << ", took: " << Logger::FIXED(executionTime());
  }
}

std::string Query::extractQueryString(size_t maxLength, bool show) const {
  if (!show) {
    return "<hidden>";
  }
  return queryString().extract(maxLength);
}

void Query::stringifyBindParameters(std::string& out, std::string_view prefix,
                                    size_t maxLength) const {
  auto bp = bindParametersAsBuilder();
  if (bp != nullptr && !bp->slice().isNone() && maxLength >= 3) {
    // append prefix, e.g. "bind parameters: "
    out.append(prefix);
    size_t const initialLength = out.size();

    // dump at most maxLength chars of bind parameters into our output string
    velocypack::SizeConstrainedStringSink sink(&out, maxLength + initialLength);
    velocypack::Dumper dumper(&sink);
    dumper.dump(bp->slice());

    if (sink.overflowed()) {
      // truncate value with "..."
      TRI_ASSERT(initialLength + maxLength >= 3);
      out.resize(initialLength + maxLength - 3);
      absl::StrAppend(&out, "... (",
                      sink.unconstrainedLength() - initialLength - maxLength,
                      ")");
    }
  }
}

void Query::stringifyDataSources(std::string& out,
                                 std::string_view prefix) const {
  auto const d = collectionNames();
  if (!d.empty()) {
    out.append(prefix);
    out.push_back('[');

    velocypack::StringSink sink(&out);
    velocypack::Dumper dumper(&sink);
    size_t i = 0;
    for (auto const& dn : d) {
      if (i > 0) {
        out.push_back(',');
      }
      dumper.appendString(dn.data(), dn.size());
      ++i;
    }
    out.push_back(']');
  }
}

/// @brief calculate a hash value for the query and bind parameters
uint64_t Query::calculateHash() const {
  if (_queryString.empty()) {
    return kDontCache;
  }

  // hash the query string first
  uint64_t hashval = _queryString.hash();

  // handle "fullCount" option. if this option is set, the query result will
  // be different to when it is not set!
  if (_queryOptions.fullCount) {
    hashval =
        fasthash64(::fullcountTrue.data(), ::fullcountTrue.size(), hashval);
  } else {
    hashval =
        fasthash64(::fullcountFalse.data(), ::fullcountFalse.size(), hashval);
  }

  // handle "count" option
  if (_queryOptions.count) {
    hashval = fasthash64(::countTrue.data(), ::countTrue.size(), hashval);
  } else {
    hashval = fasthash64(::countFalse.data(), ::countFalse.size(), hashval);
  }

  // also hash "optimizer.rules" options
  for (auto const& rule : _queryOptions.optimizerRules) {
    hashval = VELOCYPACK_HASH(rule.data(), rule.size(), hashval);
  }
  // blend query hash with bind parameters
  return hashval ^ _bindParameters.hash();
}

/// @brief whether or not the query results cache can be used for the query
bool Query::canUseResultsCache() const {
  bool isCachingAllowed = !(_transactionContext->isStreaming() ||
                            _transactionContext->isTransactionJS()) ||
                          _transactionContext->isReadOnlyTransaction();

  if (!isCachingAllowed || _queryString.size() < 8 || _queryOptions.silent) {
    return false;
  }

  auto queryCacheMode = QueryCache::instance()->mode();

  if (_queryOptions.cache && (queryCacheMode == CACHE_ALWAYS_ON ||
                              queryCacheMode == CACHE_ON_DEMAND)) {
    // cache mode is set to always on or on-demand...
    // query will only be cached if `cache` attribute is not set to false

    // cannot use query cache on a coordinator at the moment
    return !ServerState::instance()->isRunningInCluster();
  }

  return false;
}

Result Query::result() const {
  std::lock_guard<std::mutex> guard{_resultMutex};
  return _result.value_or(Result{});
}

void Query::setResult(Result&& res) noexcept {
  std::lock_guard<std::mutex> guard{_resultMutex};
  if (!_result.has_value()) {
    _result = std::move(res);
  }
}

/// @brief enter a new state
void Query::enterState(QueryExecutionState::ValueType state) {
  LOG_TOPIC("d8767", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::enterState: " << QueryExecutionState::toString(state)
      << " this: " << (uintptr_t)this;
  if (_queryProfile != nullptr) {
    // record timing for previous state
    _queryProfile->setStateDone(_execState);
  }

  // and adjust the state
  _execState = state;
}

/// @brief cleanup plan and engine for current query
ExecutionState Query::cleanupPlanAndEngine(bool sync) {
  ensureExecutionTime();

  {
    std::unique_lock<std::mutex> guard{_resultMutex};
    if (!_postProcessingDone) {
      _postProcessingDone = true;

      guard.unlock();

      try {
        handlePostProcessing();
      } catch (std::exception const& ex) {
        LOG_TOPIC("48fde", WARN, Logger::QUERIES)
            << "caught exception during query postprocessing: " << ex.what();
      }
    }
  }

  if (sync && _sharedState) {
    _sharedState->resetWakeupHandler();
    auto state = cleanupTrxAndEngines();
    while (state == ExecutionState::WAITING) {
      _sharedState->waitForAsyncWakeup();
      state = cleanupTrxAndEngines();
    }
    return state;
  }

  return cleanupTrxAndEngines();
}

void Query::unregisterSnippets() {
  if (!_snippets.empty() && ServerState::instance()->isCoordinator()) {
    auto* registry = QueryRegistryFeature::registry();
    if (registry) {
      registry->unregisterSnippets(_snippets);
    }
  }
}

void Query::handlePostProcessing(QueryList& querylist) {
  Query::QuerySerializationOptions const options{
      .includeUser = true,
      .includeQueryString = querylist.trackQueryString(),
      .includeBindParameters = querylist.trackBindVars(),
      .includeDataSources = querylist.trackDataSources(),
      // always true because the query is already finalized
      .includeResultCode = true,
      .queryStringMaxLength = querylist.maxQueryStringLength(),
  };

  // building the query slice is expensive. only do it if we actually need
  // to log the query.
  std::shared_ptr<velocypack::String> querySlice;
  auto buildQuerySlice = [&querySlice, &options, this]() {
    if (querySlice == nullptr) {
      velocypack::Builder builder;
      toVelocyPack(builder, /*isCurrent*/ false, options);

      querySlice = std::make_shared<velocypack::String>(builder.slice());
    }
    return querySlice;
  };

  // check if the query is considered a slow query and needs special treatment
  double threshold = queryOptions().stream
                         ? querylist.slowStreamingQueryThreshold()
                         : querylist.slowQueryThreshold();

  bool isSlowQuery = (querylist.trackSlowQueries() &&
                      executionTime() >= threshold && threshold >= 0.0);
  if (isSlowQuery) {
    // yes.
    try {
      TRI_IF_FAILURE("QueryList::remove") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      auto& queryRegistryFeature =
          vocbase().server().getFeature<QueryRegistryFeature>();
      queryRegistryFeature.trackSlowQuery(executionTime());
      logSlow(options);

      querylist.trackSlow(buildQuerySlice());
    } catch (...) {
    }
  }

  if (vocbase().server().hasFeature<QueryInfoLoggerFeature>()) {
    auto& qilf = vocbase().server().getFeature<QueryInfoLoggerFeature>();

    // building the query slice is expensive. only do it if we actually need
    // to log the query.
    if (qilf.shouldLog(vocbase().isSystem(), isSlowQuery)) {
      qilf.log(buildQuerySlice());
    }
  }
}

void Query::handlePostProcessing() {
  // elapsed time since query start
  auto& queryRegistryFeature =
      vocbase().server().getFeature<QueryRegistryFeature>();
  queryRegistryFeature.trackQueryEnd(executionTime());

  if (!queryOptions().skipAudit &&
      ServerState::instance()->isSingleServerOrCoordinator()) {
    try {
      auto queryList = vocbase().queryList();
      // internal queries that are excluded from audit logging will not be
      // logged here as slow queries, and will not be inserted into the
      // _queries system collection
      if (queryList != nullptr) {
        handlePostProcessing(*queryList);
      }

      // log to normal log
      logAtEnd();

      // log to audit log
      events::AqlQuery(*this);
    } catch (...) {
      // we must not make any exception escape from here!
    }
  }
}

/// @brief pass-thru a resolver object from the transaction context
CollectionNameResolver const& Query::resolver() const {
  return _transactionContext->resolver();
}

/// @brief create a transaction::Context
std::shared_ptr<transaction::Context> Query::newTrxContext() const {
  TRI_ASSERT(_transactionContext != nullptr);
  TRI_ASSERT(_trx != nullptr);

  if (_ast->canApplyParallelism() || _ast->containsAsyncPrefetch()) {
    // some degree of parallel execution. nodes should better not
    // share the transaction context, but create their own, non-shared
    // objects.
    TRI_ASSERT(!_ast->containsModificationNode());
    return _transactionContext->clone();
  }
  // no parallelism in this query. all parts can use the same
  // transaction context
  return _transactionContext;
}

velocypack::Options const& Query::vpackOptions() const {
  return *(_transactionContext->getVPackOptions());
}

transaction::Methods& Query::trxForOptimization() {
  TRI_ASSERT(_execState != QueryExecutionState::ValueType::EXECUTION);
  TRI_ASSERT(_trx != nullptr);
  return *_trx;
}

void Query::addIntermediateCommits(uint64_t value) {
  TRI_ASSERT(_trx != nullptr);
  _trx->state()->addIntermediateCommits(value);
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
void Query::initForTests() {
  this->init(/*createProfile*/ false);
  initTrxForTests();
}

void Query::initTrxForTests() {
  _trx = AqlTransaction::create(_transactionContext, _collections,
                                _queryOptions.transactionOptions,
                                std::unordered_set<std::string>{});
  // create the transaction object, but do not start it yet
  _trx->addHint(
      transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel
  auto res = _trx->begin();
  TRI_ASSERT(res.ok());
}
#endif

/// @brief return the query's shared state
std::shared_ptr<SharedQueryState> Query::sharedState() const {
  return _sharedState;
}

ExecutionEngine* Query::rootEngine() const {
  if (!_snippets.empty()) {
    TRI_ASSERT(ServerState::instance()->isDBServer() ||
               _snippets[0]->engineId() == 0);
    return _snippets[0].get();
  }
  return nullptr;
}

namespace {

futures::Future<Result> finishDBServerParts(Query& query, ErrorCode errorCode) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  auto const& serverQueryIds = query.serverQueryIds();
  TRI_ASSERT(!serverQueryIds.empty());

  auto& server = query.vocbase().server();

  NetworkFeature const& nf = server.getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (pool == nullptr) {
    return futures::makeFuture(Result{TRI_ERROR_SHUTTING_DOWN});
  }

  // used by hotbackup to prevent commits
  std::optional<transaction::Manager::TransactionCommitGuard> commitGuard;
  // If the query is not read-only, we want to acquire the transaction
  // commit lock as read lock, read-only queries can just proceed.
  // note that we only need to acquire the commit lock if the transaction
  // is actually about to commit (i.e. no error happened) and not about
  // to abort:
  if (query.isModificationQuery() && errorCode == TRI_ERROR_NO_ERROR) {
    auto* manager = server.getFeature<transaction::ManagerFeature>().manager();
    commitGuard.emplace(manager->getTransactionCommitGuard());
  }

  network::RequestOptions options;
  options.database = query.vocbase().name();
  options.timeout = network::Timeout(120.0);  // Picked arbitrarily
  switch (query.executeCallerWaiting()) {
    case QueryContext::ExecuteCallerWaiting::Asynchronously:
      options.continuationLane = RequestLane::CLUSTER_INTERNAL;
      break;
    case QueryContext::ExecuteCallerWaiting::Synchronously:
      options.skipScheduler = true;
      break;
  }

  VPackBuffer<uint8_t> body;
  VPackBuilder builder(body);
  builder.openObject(true);
  builder.add(StaticStrings::Code, VPackValue(errorCode));
  builder.close();

  query.incHttpRequests(static_cast<unsigned>(serverQueryIds.size()));

  std::vector<futures::Future<Result>> futures;
  futures.reserve(serverQueryIds.size());
  auto ss = query.sharedState();
  TRI_ASSERT(ss != nullptr);

  for (auto const& [server, queryId, rebootId] : serverQueryIds) {
    TRI_ASSERT(!server.starts_with("server:"));

    auto f =
        network::sendRequestRetry(
            pool, "server:" + server, fuerte::RestVerb::Delete,
            absl::StrCat("/_api/aql/finish/", queryId), body, options)
            .thenValue([ss, &query](network::Response&& res) mutable -> Result {
              // simon: checked until 3.5, shutdown result is always ignored
              if (res.fail()) {
                return Result{network::fuerteToArangoErrorCode(res)};
              } else if (!res.slice().isObject()) {
                return Result(TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                              "shutdown response of DBServer is malformed");
              }

              if (VPackSlice val = res.slice().get("stats"); val.isObject()) {
                ss->executeLocked([&] {
                  query.executionStatsGuard().doUnderLock(
                      [&](auto& executionStats) {
                        executionStats.add(ExecutionStats(val));
                        if (auto s = val.get("intermediateCommits");
                            s.isNumber<uint64_t>()) {
                          query.addIntermediateCommits(s.getNumber<uint64_t>());
                        }
                      });
                });
              }
              // read "warnings" attribute if present and add it to our
              // query
              if (VPackSlice val = res.slice().get("warnings"); val.isArray()) {
                for (VPackSlice it : VPackArrayIterator(val)) {
                  if (it.isObject()) {
                    VPackSlice code = it.get("code");
                    VPackSlice message = it.get("message");
                    if (code.isNumber() && message.isString()) {
                      query.warnings().registerWarning(
                          ErrorCode{code.getNumericValue<int>()},
                          message.copyString());
                    }
                  }
                }
              }

              if (VPackSlice val = res.slice().get("code"); val.isNumber()) {
                return Result{ErrorCode{val.getNumericValue<int>()}};
              }
              return Result();
            })
            .thenError<std::exception>([](std::exception ptr) {
              return Result(TRI_ERROR_INTERNAL,
                            "unhandled query shutdown exception");
            });

    futures.push_back(std::move(f));
  }

  return futures::collectAll(std::move(futures))
      .thenValue([commitGuard = std::move(commitGuard)](
                     std::vector<futures::Try<Result>>&& results) -> Result {
        for (futures::Try<Result>& tryRes : results) {
          if (tryRes.get().fail()) {
            return std::move(tryRes).get();
          }
        }
        return Result();
      });
}
}  // namespace

ExecutionState Query::cleanupTrxAndEngines() {
  ShutdownState exp = _shutdownState.load(std::memory_order_relaxed);
  if (exp == ShutdownState::Done) {
    return ExecutionState::DONE;
  } else if (exp == ShutdownState::InProgress) {
    return ExecutionState::WAITING;
  }

  TRI_ASSERT(exp == ShutdownState::None);
  if (!_shutdownState.compare_exchange_strong(exp, ShutdownState::InProgress,
                                              std::memory_order_relaxed)) {
    return ExecutionState::WAITING;  // someone else got here
  }

  ScopeGuard endQueryGuard(
      [this]() noexcept { unregisterQueryInTransactionState(); });

  enterState(QueryExecutionState::ValueType::FINALIZATION);

  TRI_IF_FAILURE("Query::directKillBeforeQueryWillBeFinalized") {
    debugKillQuery();
  }

  // simon: do not unregister _queryProfile here, since kill() will be called
  //        under the same QueryList lock

  // The above condition is not true if we have already waited.
  LOG_TOPIC("fc22c", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::finalize: before _trx->commit"
      << " this: " << (uintptr_t)this;

  // Only one thread is allowed to call commit
  if (result().ok()) {
    ScopeGuard guard([this]() noexcept {
      // If we exit here we need to throw the error.
      // The caller will handle the error and will call this method
      // again using an errorCode != NO_ERROR.
      // If we do not reset to None here, this additional call will cause
      // endless looping.
      _shutdownState.store(ShutdownState::None, std::memory_order_relaxed);
    });
    futures::Future<Result> commitResult = _trx->commitAsync();
    if (commitResult.waitAndGet().fail()) {
      THROW_ARANGO_EXCEPTION(std::move(commitResult).waitAndGet());
    }
    TRI_IF_FAILURE("Query::finalize_before_done") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    // We succeeded with commit. Let us cancel the guard
    // The state of "in progress" is now correct if we exit the method.
    guard.cancel();
  }

  TRI_IF_FAILURE("Query::directKillAfterQueryWillBeFinalized") {
    debugKillQuery();
  }

  LOG_TOPIC("7ef18", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::finalize: before cleanupPlanAndEngine"
      << " this: " << (uintptr_t)this;

  if (_serverQueryIds.empty()) {
    _shutdownState.store(ShutdownState::Done, std::memory_order_relaxed);
    return ExecutionState::DONE;
  }

  TRI_ASSERT(ServerState::instance()->isCoordinator());
  TRI_ASSERT(_sharedState);
  try {
    TRI_IF_FAILURE("Query::finalize_error_on_finish_db_servers") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    }
    ::finishDBServerParts(*this, result().errorNumber())
        .thenFinal([ss = _sharedState, this](futures::Try<Result>&& tryResult) {
          auto&& r =
              basics::catchToResult([&] { return std::move(tryResult).get(); });
          LOG_TOPIC_IF("fd31e", INFO, Logger::QUERIES,
                       r.fail() && r.isNot(TRI_ERROR_HTTP_NOT_FOUND))
              << "received error from DBServer on query finalization: "
              << r.errorNumber() << ", '" << r.errorMessage() << "'";
          _sharedState->executeAndWakeup([&] {
            _shutdownState.store(ShutdownState::Done,
                                 std::memory_order_relaxed);
            return true;
          });
        });

    TRI_IF_FAILURE("Query::directKillAfterDBServerFinishRequests") {
      debugKillQuery();
    }

    return ExecutionState::WAITING;
  } catch (...) {
    // In case of any error that happened in sending out the requests
    // we simply reset to done, we tried to cleanup the engines.
    // we only get here if something in the network stack is out of order.
    // so there is no need to retry on cleaning up the engines, caller can
    // continue Also note: If an error in cleanup happens the query was
    // completed already, so this error does not need to be reported to
    // client.
    _shutdownState.store(ShutdownState::Done, std::memory_order_relaxed);

    if (isModificationQuery()) {
      // For modification queries these left-over locks will have negative
      // side effects We will report those to the user. Lingering Read-locks
      // should not block the system.
      std::vector<std::string_view> writeLocked{};
      std::vector<std::string_view> exclusiveLocked{};
      _collections.visit([&](std::string const& name, Collection& col) -> bool {
        switch (col.accessType()) {
          case AccessMode::Type::WRITE: {
            writeLocked.emplace_back(name);
            break;
          }
          case AccessMode::Type::EXCLUSIVE: {
            exclusiveLocked.emplace_back(name);
            break;
          }
          default:
            // We do not need to capture reads.
            break;
        }
        return true;
      });
      LOG_TOPIC("63572", WARN, Logger::QUERIES)
          << " Failed to cleanup leftovers of a query due to communication "
             "errors. "
          << " The DBServers will eventually clean up the state. The "
             "following locks still exist: write: "
          << writeLocked
          << ": you may not drop these collections until the locks time out."
          << " exclusive: " << exclusiveLocked
          << ": you may not be able to write into these collections until "
             "the locks time out.";

      for (auto const& [server, queryId, rebootId] : _serverQueryIds) {
        // note: if the text structure of this message is changed, it is likely
        // that some test in tests/js/client/shell/aql-failures-cluster.js also
        // needs to be adjusted to honor the new message structure.
        auto msg = absl::StrCat(
            "Failed to send unlock request DELETE /_api/aql/finish/", queryId,
            " to server:", server, " in database ", vocbase().name());
        LOG_TOPIC("7c10f", WARN, Logger::QUERIES) << msg;
        _warnings.registerWarning(TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                                  std::move(msg));
      }
    }
    return ExecutionState::DONE;
  }
}

void Query::injectVertexCollectionIntoGraphNodes(ExecutionPlan& plan) {
  containers::SmallVector<ExecutionNode*, 8> graphNodes;

  plan.findNodesOfType(graphNodes,
                       {ExecutionNode::TRAVERSAL, ExecutionNode::SHORTEST_PATH,
                        ExecutionNode::ENUMERATE_PATHS},
                       true);
  for (auto& node : graphNodes) {
    auto graphNode = ExecutionNode::castTo<GraphNode*>(node);
    auto const& vCols = graphNode->vertexColls();

    if (vCols.empty()) {
      auto& myResolver = resolver();

      // In case our graphNode does not have any collections added yet,
      // we need to visit all query-known collections and add the
      // vertex collections.
      collections().visit([&myResolver, graphNode](std::string const& name,
                                                   Collection& collection) {
        // If resolver cannot resolve this collection
        // it has to be a view.
        if (myResolver.getCollection(name)) {
          // All known edge collections will be ignored by this call!
          graphNode->injectVertexCollection(collection);
        }
        return true;
      });
    }
  }
}

void Query::debugKillQuery() {
#ifndef ARANGODB_ENABLE_FAILURE_TESTS
  TRI_ASSERT(false);
#else
  if (_wasDebugKilled) {
    return;
  }
  bool usingSystemCollection = false;
  // Ignore queries on System collections, we do not want them to hit failure
  // points note that we must call the _const_ version of collections() here,
  // because the non-const version will trigger an assertion failure if the
  // query is already executing!
  const_cast<Query const*>(this)->collections().visit(
      [&usingSystemCollection](std::string const&,
                               Collection const& col) -> bool {
        if (col.getCollection()->system()) {
          usingSystemCollection = true;
          return false;
        }
        return true;
      });

  if (usingSystemCollection) {
    return;
  }

  _wasDebugKilled = true;
  // A query can only be killed under certain circumstances.
  // We assert here that one of those is true.
  // a) Query is in the list of current queries, this can be requested by the
  // user and the query can be killed by user b) Query is in the query
  // registry. In this case the query registry can hit a timeout, which
  // triggers the kill c) The query id has been handed out to the user (stream
  // query only)
  bool isStreaming = queryOptions().stream;
  bool isInList = false;
  bool isInRegistry = false;
  auto const& queryList = vocbase().queryList();
  if (queryList->enabled()) {
    auto const& current = queryList->listCurrent();
    for (auto const& it : current) {
      auto slice = it->slice();
      TRI_ASSERT(slice.isObject());
      TRI_ASSERT(slice.hasKey("id"));
      if (auto id = slice.get("id");
          id.isString() && id.stringView() == std::to_string(_queryId)) {
        isInList = true;
        break;
      }
    }
  }

  QueryRegistry* registry = QueryRegistryFeature::registry();
  if (registry != nullptr) {
    isInRegistry = registry->queryIsRegistered(_queryId);
  }
  TRI_ASSERT(isInList || isStreaming || isInRegistry ||
             _execState == QueryExecutionState::ValueType::FINALIZATION)
      << "_execState " << (int)_execState.load() << " queryList->enabled() "
      << queryList->enabled();
  kill();
#endif
}

/// @brief prepare a query out of some velocypack data.
/// only to be used on single server or coordinator.
/// never call this on a DB server!
void Query::prepareFromVelocyPackWithoutInstantiate(
    velocypack::Slice querySlice, velocypack::Slice collections,
    velocypack::Slice views, velocypack::Slice variables,
    velocypack::Slice snippets) {
  // Note that the `views` slice can either be None or a list of views.
  // Both usages are allowed and are used in the code!
  TRI_ASSERT(!ServerState::instance()->isDBServer());

  LOG_TOPIC("9636f", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::prepareFromVelocyPackWithoutInstantiate"
      << " this: " << (uintptr_t)this;

  // track memory usage
  ResourceUsageScope scope(*_resourceMonitor);
  scope.increase(querySlice.byteSize() + collections.byteSize() +
                 variables.byteSize() + snippets.byteSize());

  _planMemoryUsage += scope.trackedAndSteal();

  init(/*createProfile*/ true);

  if (auto val = querySlice.get("isModificationQuery"); val.isTrue()) {
    _ast->setContainsModificationNode();
  }
  if (auto val = querySlice.get("isAsyncQuery"); val.isTrue()) {
    _ast->setContainsParallelNode();
  }

  enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);

  ExecutionPlan::getCollectionsFromVelocyPack(_collections, collections);
  if (views.isArray()) {
    ExecutionPlan::extendCollectionsByViewsFromVelocyPack(_collections, views);
  }

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
#endif

  _trx = AqlTransaction::create(_transactionContext, _collections,
                                _queryOptions.transactionOptions,
                                std::move(inaccessibleCollections));

  // create the transaction object, but do not start the transaction yet
  _trx->addHint(
      transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel

  _allowDirtyReads = _trx->state()->options().allowDirtyReads;

  Result res = _trx->begin();
  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  TRI_IF_FAILURE("Query::setupLockTimeout") {
    if (!_trx->state()->isReadOnlyTransaction() &&
        RandomGenerator::interval(uint32_t(100)) >= 95) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_LOCK_TIMEOUT);
    }
  }

  enterState(QueryExecutionState::ValueType::PARSING);
}

async<void> Query::instantiatePlan(velocypack::Slice snippets) {
  bool const planRegisters = !_queryString.empty();
  auto instantiateSnippet = [&](velocypack::Slice snippet) -> async<void> {
    auto plan =
        ExecutionPlan::instantiateFromVelocyPack(_ast.get(), snippet, true);
    TRI_ASSERT(plan != nullptr);

    co_await ExecutionEngine::instantiateFromPlan(*this, *plan, planRegisters);
    _plans.push_back(std::move(plan));
    co_return;
  };

  // a single snippet
  co_await instantiateSnippet(snippets);
  TRI_ASSERT(!_snippets.empty());
  TRI_ASSERT(!_trx->state()->isDBServer() || _snippets.back()->engineId() != 0);

  TRI_ASSERT(!_snippets.empty());

  if (!_snippets.empty()) {
    TRI_ASSERT(_trx->state()->isDBServer() || _snippets[0]->engineId() == 0);
    // simon: just a hack for AQL_EXECUTEJSON
    if (_trx->state()->isCoordinator()) {  // register coordinator snippets
      TRI_ASSERT(_trx->state()->isCoordinator());
      QueryRegistryFeature::registry()->registerSnippets(_snippets);
    }

    registerQueryInTransactionState();
  }

  _queryProfile->registerInQueryList();

  auto& feature = vocbase().server().getFeature<QueryRegistryFeature>();
  feature.trackQueryStart();

  enterState(QueryExecutionState::ValueType::EXECUTION);

  co_return;
}

auto aql::toString(Query::ExecutionPhase phase) -> std::string_view {
  switch (phase) {
    case Query::ExecutionPhase::INITIALIZE:
      return "INITIALIZE";
    case Query::ExecutionPhase::PREPARE:
      return "PREPARE";
    case Query::ExecutionPhase::EXECUTE:
      return "EXECUTE";
    case Query::ExecutionPhase::FINALIZE:
      return "FINALIZE";
  }
  ADB_UNREACHABLE;
}

void Query::toVelocyPack(velocypack::Builder& builder, bool isCurrent,
                         QuerySerializationOptions const& options) const {
  // query state
  auto currentState = std::invoke([&]() {
    if (killed()) {
      return QueryExecutionState::ValueType::KILLED;
    }
    return (isCurrent ? state() : QueryExecutionState::ValueType::FINISHED);
  });

  double elapsed = (isCurrent ? elapsedSince(startTime()) : executionTime());

  double now = TRI_microtime();
  // we calculate the query start timestamp as the current time minus
  // the elapsed time since query start. this is not 100% accurrate, but
  // best effort, and saves us from bookkeeping the start timestamp of the
  // query inside the Query object.
  TRI_ASSERT(now >= elapsed);

  builder.openObject();

  // query id (note: this is always returned as a string)
  builder.add("id", VPackValue(std::to_string(id())));

  // database name
  builder.add("database", VPackValue(vocbase().name()));

  // user
  if (options.includeUser) {
    builder.add("user", VPackValue(user()));
  }
  // query string
  builder.add("query",
              VPackValue(extractQueryString(options.queryStringMaxLength,
                                            options.includeQueryString)));

  // bind parameters
  if (options.includeBindParameters) {
    if (auto b = bindParametersAsBuilder();
        b != nullptr && !b->slice().isNone()) {
      builder.add("bindVars", b->slice());
    } else {
      builder.add("bindVars", velocypack::Slice::emptyObjectSlice());
    }
  } else {
    builder.add("bindVars", velocypack::Slice::emptyObjectSlice());
  }

  // data sources
  if (options.includeDataSources) {
    builder.add("dataSources", VPackValue(VPackValueType::Array));
    for (auto const& ds : collectionNames()) {
      builder.add(VPackValue(ds));
    }
    builder.close();
  }

  // start time
  auto timeString =
      TRI_StringTimeStamp(/*started*/ now - elapsed, Logger::getUseLocalTime());
  builder.add("started", VPackValue(timeString));

  // run time
  builder.add("runTime", VPackValue(elapsed));

  // peak memory usage
  builder.add("peakMemoryUsage", VPackValue(resourceMonitor().peak()));

  // state
  builder.add("state", VPackValue(QueryExecutionState::toString(currentState)));

  // streaming yes/no?
  builder.add("stream", VPackValue(queryOptions().stream));

  // modification query yes/no?
  builder.add("modificationQuery", VPackValue(isModificationQuery()));

#if 0
  // TODO: currently does not work in cluster, as stats in cluster are only 
  // updated after the query is removed from the query list.
  // these attributes can be added once the issue is fixed in cluster.
  builder.add("writesExecuted", VPackValue(_execStats.writesExecuted));
  builder.add("writesIgnored", VPackValue(_execStats.writesIgnored));
  builder.add("documentLookups", VPackValue(_execStats.documentLookups));
  builder.add("scannedFull", VPackValue(_execStats.scannedFull));
  builder.add("scannedIndex", VPackValue(_execStats.scannedIndex));
  builder.add("cacheHits", VPackValue(_execStats.cacheHits));
  builder.add("cacheMisses", VPackValue(_execStats.cacheMisses));
  builder.add("filtered", VPackValue(_execStats.filtered));
  builder.add("requests", VPackValue(_execStats.requests));
  builder.add("intermediateCommits",
              VPackValue(_execStats.intermediateCommits));
  builder.add("count", VPackValue(_execStats.count));
#endif

  // number of warnings
  builder.add("warnings", VPackValue(warnings().count()));

  // exit code
  if (options.includeResultCode) {
    // exit code can only be determined if query is fully finished
    builder.add("exitCode", VPackValue(result().errorNumber()));
  }

  builder.close();
}

void Query::logSlow(QuerySerializationOptions const& options) const {
  auto buildBindParameters = [&]() {
    std::string bindParameters;
    if (options.includeBindParameters) {
      // also log bind variables
      stringifyBindParameters(bindParameters,
                              ", bind vars: ", options.queryStringMaxLength);
    }
    return bindParameters;
  };

  auto buildDataSources = [&]() {
    std::string dataSources;
    if (options.includeDataSources) {
      stringifyDataSources(dataSources, ", data sources: ");
    }
    return dataSources;
  };

  LOG_TOPIC("8bcee", WARN, Logger::QUERIES)
      << "slow " << (queryOptions().stream ? "streaming " : "") << "query: '"
      << extractQueryString(options.queryStringMaxLength,
                            options.includeQueryString)
      << "'" << buildBindParameters() << buildDataSources()
      << ", database: " << vocbase().name() << ", user: " << user()
      << ", id: " << id() << ", token: QRY" << id()
      << ", peak memory usage: " << resourceMonitor().peak()
      << ", exit code: " << result().errorNumber()
      << ", took: " << Logger::FIXED(executionTime()) << " s";
}

std::function<void(velocypack::Builder&)>
Query::buildSerializeQueryDataCallback(
    Query::CollectionSerializationFlags flags) const {
  return [flags, this](velocypack::Builder& builder) {
    // set up collections
    TRI_ASSERT(builder.isOpenObject());
    builder.add(VPackValue("collections"));
    collections().toVelocyPack(
        builder,
        /*filter*/ [flags, this](std::string const& name, Collection const&) {
          // exclude collections without names or with names that are just ids
          if (name.empty()) {
            return false;
          }
          if (!flags.includeNumericIds &&
              (name.front() >= '0' && name.front() <= '9')) {
            // exclude numeric collection ids from the serialization.
            return false;
          }
          if (!flags.includeViews &&
              this->resolver().getCollection(name) == nullptr) {
            // collection does not exist. probably a view.
            return false;
          }
          return true;
        });
    // Views separately, if requested:
    if (flags.includeViewsSeparately) {
      builder.add(VPackValue("views"));
      collections().toVelocyPack(
          builder,
          /*filter*/ [flags, this](std::string const& name, Collection const&) {
            if (name.empty()) {
              return false;
            }
            if (!flags.includeNumericIds &&
                (name.front() >= '0' && name.front() <= '9')) {
              // exclude numeric collection ids from the serialization.
              return false;
            }
            return this->resolver().getView(name) != nullptr;
          });
    }

    // set up variables
    TRI_ASSERT(builder.isOpenObject());
    builder.add(VPackValue("variables"));
    _ast->variables()->toVelocyPack(builder);

    builder.add("isModificationQuery",
                VPackValue(_ast->containsModificationNode()));
  };
}
