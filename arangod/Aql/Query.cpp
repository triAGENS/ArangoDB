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
#include "Aql/ClusterQuery.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/GraphNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Optimizer.h"
#include "Aql/Parser.h"
#include "Aql/ProfileLevel.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryExecutionState.h"
#include "Aql/QueryList.h"
#include "Aql/QueryProfile.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Aql/Timing.h"
#include "Basics/Exceptions.h"
#include "Basics/ResourceUsage.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/fasthash.h"
#include "Cluster/ServerState.h"
#include "Graph/Graph.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Random/RandomGenerator.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/StandaloneContext.h"
#ifdef USE_V8
#include "Transaction/V8Context.h"
#endif
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "Utils/Events.h"
#ifdef USE_V8
#include "V8/JavaScriptSecurityContext.h"
#include "V8/v8-vpack.h"
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

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
  std::optional<arangodb::transaction::Manager::TransactionCommitGuard>
      commitGuard;
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
  options.continuationLane = RequestLane::CLUSTER_INTERNAL;
  // Most coordinator AQL code might be executed on the MEDIUM prio lane,
  // since it comes from a continuation. Therefore, to avoid deadlock, we
  // must use a lane which has priority HIGH. We do not want to skip the
  // scheduler, since the cleanup code acquires locks and does some
  // non-trivial work.

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

    futures.emplace_back(std::move(f));
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
      _queryHash(DontCache),
      _shutdownState(ShutdownState::None),
      _executionPhase(ExecutionPhase::INITIALIZE),
      _resultCode(std::nullopt),
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
  _warnings.updateOptions(_queryOptions);

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
  if (_planSliceCopy != nullptr) {
    _resourceMonitor->decreaseMemoryUsage(_planSliceCopy->size());
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
  ensureExecutionTime();
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

  // log to audit log
  if (!_queryOptions.skipAudit &&
      ServerState::instance()->isSingleServerOrCoordinator()) {
    try {
      events::AqlQuery(*this);
    } catch (...) {
      // we must not make any exception escape from here!
    }
  }

  ErrorCode resultCode = TRI_ERROR_INTERNAL;
  if (killed()) {
    resultCode = TRI_ERROR_QUERY_KILLED;
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
    cleanupPlanAndEngine(TRI_ERROR_QUERY_KILLED);
  }
}

void Query::setKillFlag() {
  _queryKilled.store(true, std::memory_order_acq_rel);
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

void Query::prepareQuery(std::shared_ptr<QueryAborter> queryAborter) {
  try {
    init(/*createProfile*/ true);

    enterState(QueryExecutionState::ValueType::PARSING);

    std::unique_ptr<ExecutionPlan> plan = preparePlan();
    TRI_ASSERT(plan != nullptr);
    plan->findVarUsage();

    TRI_ASSERT(_trx != nullptr);
    TRI_ASSERT(_trx->status() == transaction::Status::RUNNING);

    // keep serialized copy of unchanged plan to include in query profile
    // necessary because instantiate / execution replace vars and blocks
    bool const keepPlan =
        _queryOptions.profile >= ProfileLevel::Blocks &&
        ServerState::isSingleServerOrCoordinator(_trx->state()->serverRole());
    if (keepPlan) {
      auto serializeQueryData = [this](velocypack::Builder& builder) {
        // set up collections
        TRI_ASSERT(builder.isOpenObject());
        builder.add(VPackValue("collections"));
        collections().toVelocyPack(builder);

        // set up variables
        TRI_ASSERT(builder.isOpenObject());
        builder.add(VPackValue("variables"));
        _ast->variables()->toVelocyPack(builder);

        builder.add("isModificationQuery",
                    VPackValue(_ast->containsModificationNode()));
      };

      unsigned flags = ExecutionPlan::buildSerializationFlags(
          /*verbose*/ false, /*includeInternals*/ false,
          /*explainRegisters*/ false);
      _planSliceCopy = std::make_unique<VPackBufferUInt8>();
      try {
        VPackBuilder b(*_planSliceCopy);
        plan->toVelocyPack(b, flags, serializeQueryData);

        TRI_IF_FAILURE("Query::serializePlans1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
        _resourceMonitor->increaseMemoryUsage(_planSliceCopy->size());
      } catch (std::exception const& ex) {
        // must clear _planSliceCopy here so that the destructor of
        // Query doesn't subtract the memory used by _planSliceCopy
        // without us having it tracked properly here.
        _planSliceCopy.reset();
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
    ExecutionEngine::instantiateFromPlan(*this, *plan, planRegisters,
                                         queryAborter);

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
    _resultCode = ex.code();
    throw;
  } catch (std::bad_alloc const&) {
    _resultCode = TRI_ERROR_OUT_OF_MEMORY;
    throw;
  } catch (...) {
    _resultCode = TRI_ERROR_INTERNAL;
    throw;
  }
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

  // put in collection and attribute name bind parameters (e.g. @@collection or
  // doc.@attr).
  _ast->injectBindParametersFirstStage(_bindParameters, this->resolver());

  // put in value bind parameters. TODO: move this further down in the process,
  // so that the optimizer can run with value bind parameters still unreplaced
  // in the AST.
  _ast->injectBindParametersSecondStage(_bindParameters);

  if (parser.ast()->containsUpsertNode()) {
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

  return plan;
}

/// @brief execute an AQL query
ExecutionState Query::execute(std::shared_ptr<QueryAborter> aborter,
                              QueryResult& queryResult) {
  LOG_TOPIC("e8ed7", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::execute"
      << " this: " << (uintptr_t)this;

  try {
    if (killed()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    }

    bool useQueryCache = canUseQueryCache();
    switch (_executionPhase) {
      case ExecutionPhase::INITIALIZE: {
        if (useQueryCache) {
          // check the query cache for an existing result
          auto cacheEntry = QueryCache::instance()->lookup(
              &_vocbase, hash(), _queryString, bindParameters());

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

        // will throw if it fails
        if (!_ast) {  // simon: hack for AQL_EXECUTEJSON
          prepareQuery(aborter);
        }

        logAtStart();
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
        TRI_ASSERT(_trx != nullptr);

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
              hash(), _queryString, queryResult.data, bindParameters(),
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

        logAtEnd(queryResult);
        return state;
      }
    }
    // We should not be able to get here
    TRI_ASSERT(false);
  } catch (Exception const& ex) {
    queryResult.reset(Result(
        ex.code(), "AQL: " + ex.message() +
                       QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (std::bad_alloc const&) {
    queryResult.reset(
        Result(TRI_ERROR_OUT_OF_MEMORY,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                   QueryExecutionState::toStringWithPrefix(_execState))));
  } catch (std::exception const& ex) {
    queryResult.reset(Result(
        TRI_ERROR_INTERNAL,
        ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (...) {
    queryResult.reset(
        Result(TRI_ERROR_INTERNAL,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_INTERNAL),
                   QueryExecutionState::toStringWithPrefix(_execState))));
  }

  logAtEnd(queryResult);
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
QueryResult Query::executeSync(std::shared_ptr<QueryAborter> aborter) {
  std::shared_ptr<SharedQueryState> ss;

  QueryResult queryResult;
  do {
    auto state = execute(aborter, queryResult);
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
QueryResultV8 Query::executeV8(v8::Isolate* isolate,
                               std::shared_ptr<QueryAborter> aborter) {
  LOG_TOPIC("6cac7", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::executeV8"
      << " this: " << (uintptr_t)this;

  QueryResultV8 queryResult;

  try {
    bool useQueryCache = canUseQueryCache();

    if (useQueryCache) {
      // check the query cache for an existing result
      auto cacheEntry = QueryCache::instance()->lookup(
          &_vocbase, hash(), _queryString, bindParameters());

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
    prepareQuery(aborter);

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
          hash(), _queryString, builder, bindParameters(),
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
    queryResult.reset(Result(
        ex.code(), "AQL: " + ex.message() +
                       QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (std::bad_alloc const&) {
    queryResult.reset(
        Result(TRI_ERROR_OUT_OF_MEMORY,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                   QueryExecutionState::toStringWithPrefix(_execState))));
  } catch (std::exception const& ex) {
    queryResult.reset(Result(
        TRI_ERROR_INTERNAL,
        ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (...) {
    queryResult.reset(
        Result(TRI_ERROR_INTERNAL,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_INTERNAL),
                   QueryExecutionState::toStringWithPrefix(_execState))));
  }

  logAtEnd(queryResult);
  return queryResult;
}
#endif

ExecutionState Query::finalize(VPackBuilder& extras) {
  ensureExecutionTime();

  if (_queryProfile != nullptr &&
      _shutdownState.load(std::memory_order_relaxed) == ShutdownState::None) {
    // the following call removes the query from the list of currently
    // running queries. so whoever fetches that list will not see a Query that
    // is about to shut down/be destroyed
    _queryProfile->unregisterFromQueryList();
  }

  auto state = cleanupPlanAndEngine(TRI_ERROR_NO_ERROR);
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

    if (_planSliceCopy) {
      extras.add("plan", VPackSlice(_planSliceCopy->data()));
    }
  }

  double now = currentSteadyClockValue();
  if (_queryProfile != nullptr &&
      _queryOptions.profile >= ProfileLevel::Basic) {
    _queryProfile->setStateEnd(QueryExecutionState::ValueType::FINALIZATION,
                               now);
    _queryProfile->toVelocyPack(extras);
  }
  extras.close();

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
  } catch (...) {
    result.reset(Result(TRI_ERROR_INTERNAL,
                        "an unknown error occurred while parsing the query"));
  }

  TRI_ASSERT(result.fail());
  return result;
}

/// @brief explain an AQL query
QueryResult Query::explain() {
  QueryResult result;

  try {
    init(/*createProfile*/ false);
    enterState(QueryExecutionState::ValueType::PARSING);

    Parser parser(*this, *_ast, _queryString);
    parser.parse();

    // put in bind parameters
    parser.ast()->injectBindParametersFirstStage(_bindParameters,
                                                 this->resolver());
    parser.ast()->injectBindParametersSecondStage(_bindParameters);
    _bindParameters.validateAllUsed();

    // optimize and validate the ast
    enterState(QueryExecutionState::ValueType::AST_OPTIMIZATION);

    // create the transaction object
    _trx = AqlTransaction::create(_transactionContext, _collections,
                                  _queryOptions.transactionOptions);

    Result res = _trx->begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);
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

    auto serializeQueryData = [this](velocypack::Builder& builder) {
      // set up collections
      TRI_ASSERT(builder.isOpenObject());
      builder.add(VPackValue("collections"));
      collections().toVelocyPack(builder);

      // set up variables
      TRI_ASSERT(builder.isOpenObject());
      builder.add(VPackValue("variables"));
      _ast->variables()->toVelocyPack(builder);

      builder.add("isModificationQuery",
                  VPackValue(_ast->containsModificationNode()));
    };

    VPackOptions options;
    options.checkAttributeUniqueness = false;
    options.buildUnindexedArrays = true;
    result.data = std::make_shared<VPackBuilder>(&options);

    ResourceUsageScope scope(*_resourceMonitor);

    if (_queryOptions.allPlans) {
      VPackArrayBuilder guard(result.data.get());

      size_t previousSize = result.data->bufferRef().byteSize();
      auto const& plans = opt.getPlans();
      for (auto& it : plans) {
        auto& pln = it.first;
        TRI_ASSERT(pln != nullptr);

        preparePlanForSerialization(pln);

        pln->toVelocyPack(*result.data, flags, serializeQueryData);
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
      bestPlan->toVelocyPack(*result.data, flags, serializeQueryData);

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
    }

    // the query object no owns the memory used by the plan(s)
    _planMemoryUsage += scope.trackedAndSteal();

    // technically no need to commit, as we are only explaining here
    auto commitResult = _trx->commit();
    if (commitResult.fail()) {
      THROW_ARANGO_EXCEPTION(commitResult);
    }

    result.extra = std::make_shared<VPackBuilder>();
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
        ex.message() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (std::bad_alloc const&) {
    result.reset(
        Result(TRI_ERROR_OUT_OF_MEMORY,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                   QueryExecutionState::toStringWithPrefix(_execState))));
  } catch (std::exception const& ex) {
    result.reset(Result(
        TRI_ERROR_INTERNAL,
        ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (...) {
    result.reset(
        Result(TRI_ERROR_INTERNAL,
               StringUtils::concatT(
                   TRI_errno_string(TRI_ERROR_INTERNAL),
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
  _ast = std::make_unique<Ast>(*this);
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

/// @brief log a query
void Query::logAtStart() {
  if (_queryString.empty()) {
    return;
  }
  if (!vocbase().server().hasFeature<QueryRegistryFeature>()) {
    return;
  }
  auto const& feature = vocbase().server().getFeature<QueryRegistryFeature>();
  size_t maxLength = feature.maxQueryStringLength();

  LOG_TOPIC("8a86a", TRACE, Logger::QUERIES)
      << "executing query " << _queryId << ": '"
      << _queryString.extract(maxLength) << "'";
}

void Query::logAtEnd(QueryResult const& queryResult) const {
  if (_queryString.empty()) {
    // nothing to log
    return;
  }
  if (!vocbase().server().hasFeature<QueryRegistryFeature>()) {
    return;
  }

  auto const& feature = vocbase().server().getFeature<QueryRegistryFeature>();

  // log failed queries?
  bool logFailed = feature.logFailedQueries() && queryResult.result.fail();
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
        << " failed with exit code " << queryResult.result.errorNumber() << ": "
        << queryResult.result.errorMessage()
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

auto Query::completeLeases() -> void {
  for (auto& lease : _leasesFromRemote) {
    lease.cancel();
  }
  _leasesFromRemote.clear();
  for (auto& lease : _leasesToRemote) {
    lease.cancel();
  }
  _leasesToRemote.clear();
}

void Query::stringifyBindParameters(std::string& out, std::string_view prefix,
                                    size_t maxLength) const {
  auto bp = bindParameters();
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
      out.append("... (", 5);
      basics::StringUtils::itoa(
          sink.unconstrainedLength() - initialLength - maxLength, out);
      out.append(")", 1);
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
    return DontCache;
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

/// @brief whether or not the query cache can be used for the query
bool Query::canUseQueryCache() const {
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

ErrorCode Query::resultCode() const noexcept {
  // never return negative value from here
  return _resultCode.value_or(TRI_ERROR_NO_ERROR);
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
ExecutionState Query::cleanupPlanAndEngine(ErrorCode errorCode) {
  ensureExecutionTime();

  if (!_resultCode.has_value()) {  // TODO possible data race here
    // result code not yet set.
    _resultCode = errorCode;
  }

  return cleanupTrxAndEngines(errorCode);
}

void Query::unregisterSnippets() {
  if (!_snippets.empty() && ServerState::instance()->isCoordinator()) {
    auto* registry = QueryRegistryFeature::registry();
    if (registry) {
      registry->unregisterSnippets(_snippets);
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

ExecutionState Query::cleanupTrxAndEngines(ErrorCode errorCode) {
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
  if (errorCode == TRI_ERROR_NO_ERROR) {
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
    std::ignore =
        ::finishDBServerParts(*this, errorCode)
            .thenValue([ss = _sharedState, this](Result r) {
              LOG_TOPIC_IF("fd31e", INFO, Logger::QUERIES,
                           r.fail() && r.isNot(TRI_ERROR_HTTP_NOT_FOUND))
                  << "received error from DBServer on query finalization: "
                  << r.errorNumber() << ", '" << r.errorMessage() << "'";
              _sharedState->executeAndWakeup([&] {
                if (r.ok()) {
                  // Query was successfully finished.
                  // Let us complete the leases.
                  this->completeLeases();
                }
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
             "following "
             "locks still exist: "
          << " write: " << writeLocked
          << ": you may not drop these collections until the locks time out."
          << " exclusive: " << exclusiveLocked
          << ": you may not be able to write into these collections until "
             "the "
             "locks time out.";

      for (auto const& [server, queryId, rebootId] : _serverQueryIds) {
        auto msg = "Failed to send unlock request DELETE /_api/aql/finish/" +
                   std::to_string(queryId) + " to server:" + server +
                   " in database " + vocbase().name();
        _warnings.registerWarning(TRI_ERROR_CLUSTER_AQL_COMMUNICATION, msg);
        LOG_TOPIC("7c10f", WARN, Logger::QUERIES) << msg;
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
      if (it.id == _queryId) {
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
void Query::prepareFromVelocyPack(
    velocypack::Slice querySlice, velocypack::Slice collections,
    velocypack::Slice variables, velocypack::Slice snippets,
    QueryAnalyzerRevisions const& analyzersRevision,
    std::shared_ptr<QueryAborter> queryAborter) {
  TRI_ASSERT(!ServerState::instance()->isDBServer());

  LOG_TOPIC("9636f", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime) << " Query::prepareFromVelocyPack"
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

  bool const planRegisters = !_queryString.empty();
  auto instantiateSnippet = [&](velocypack::Slice snippet) {
    auto plan = ExecutionPlan::instantiateFromVelocyPack(_ast.get(), snippet);
    TRI_ASSERT(plan != nullptr);

    ExecutionEngine::instantiateFromPlan(*this, *plan, planRegisters,
                                         queryAborter);
    _plans.push_back(std::move(plan));
  };

  for (auto pair : VPackObjectIterator(snippets, /*sequential*/ true)) {
    instantiateSnippet(pair.value);

    TRI_ASSERT(!_snippets.empty());
    TRI_ASSERT(!_trx->state()->isDBServer() ||
               _snippets.back()->engineId() != 0);
  }

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

  enterState(QueryExecutionState::ValueType::EXECUTION);
}
