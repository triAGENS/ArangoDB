////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
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

#include "Aql/AqlCallList.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlTransaction.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Optimizer.h"
#include "Aql/Parser.h"
#include "Aql/PlanCache.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryProfile.h"
#include "Aql/QueryRegistry.h"
#include "Aql/Timing.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/fasthash.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "Graph/Graph.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "V8/JavaScriptSecurityContext.h"
#include "V8/v8-vpack.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <velocypack/Iterator.h>

#include <optional>

#ifndef USE_PLAN_CACHE
#undef USE_PLAN_CACHE
#endif

using namespace arangodb;
using namespace arangodb::aql;

/// @brief internal constructor, Used to construct a full query or a ClusterQuery
Query::Query(std::shared_ptr<transaction::Context> const& ctx,
             QueryString const& queryString, std::shared_ptr<VPackBuilder> const& bindParameters,
             aql::QueryOptions&& options,
             std::shared_ptr<SharedQueryState> sharedState)
    : QueryContext(ctx->vocbase()),
      _itemBlockManager(&_resourceMonitor, SerializationFormat::SHADOWROWS),
      _queryString(queryString),
      _transactionContext(ctx),
      _sharedState(std::move(sharedState)),
      _v8Context(nullptr),
      _bindParameters(bindParameters),
      _queryOptions(std::move(options)),
      _trx(nullptr),
      _startTime(currentSteadyClockValue()),
      _queryHash(DontCache),
      _executionPhase(ExecutionPhase::INITIALIZE),
      _contextOwnedByExterior(ctx->isV8Context() && v8::Isolate::GetCurrent() != nullptr),
      _killState(KillState::None),
      _queryHashCalculated(false) {

  if (!_transactionContext) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "failed to create query transaction context");
  }

  if (_contextOwnedByExterior) {
    // copy transaction options from global state into our local query options
    auto state = transaction::V8Context::getParentState();
    if (state != nullptr) {
      _queryOptions.transactionOptions = state->options();
    }
  }

  ProfileLevel level = _queryOptions.profile;
  if (level >= PROFILE_LEVEL_TRACE_1) {
    LOG_TOPIC("22a70", INFO, Logger::QUERIES) << elapsedSince(_startTime)
                                              << " Query::Query queryString: " << _queryString
                                              << " this: " << (uintptr_t)this;
  } else {
    LOG_TOPIC("11160", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " Query::Query queryString: " << _queryString << " this: " << (uintptr_t)this;
  }

  if (bindParameters != nullptr && !bindParameters->isEmpty() &&
      !bindParameters->slice().isNone()) {
    if (level >= PROFILE_LEVEL_TRACE_1) {
      LOG_TOPIC("8c9fc", INFO, Logger::QUERIES)
          << "bindParameters: " << bindParameters->slice().toJson();
    } else {
      LOG_TOPIC("16a3f", DEBUG, Logger::QUERIES)
          << "bindParameters: " << bindParameters->slice().toJson();
    }
  }

  if (level >= PROFILE_LEVEL_TRACE_1) {
    VPackBuilder b;
    _queryOptions.toVelocyPack(b, /*disableOptimizerRules*/ false);
    LOG_TOPIC("8979d", INFO, Logger::QUERIES) << "options: " << b.toJson();
  }

  _resourceMonitor.setMemoryLimit(_queryOptions.memoryLimit);
  _warnings.updateOptions(_queryOptions);
}

/// @brief public constructor, Used to construct a full query
Query::Query(std::shared_ptr<transaction::Context> const& ctx,
             QueryString const& queryString, std::shared_ptr<VPackBuilder> const& bindParameters,
             QueryOptions&& options)
    : Query(ctx, queryString, bindParameters, std::move(options),
            std::make_shared<SharedQueryState>(ctx->vocbase().server())) {}

Query::Query(std::shared_ptr<transaction::Context> const& ctx,
             QueryString const& queryString, std::shared_ptr<VPackBuilder> const& bindParameters,
             std::shared_ptr<VPackBuilder> const& options)
    : Query(ctx, queryString, bindParameters,
            QueryOptions(options != nullptr ? options->slice() : VPackSlice()),
                         std::make_shared<SharedQueryState>(ctx->vocbase().server())) {}

/// @brief destroys a query
Query::~Query() {
  if (_queryOptions.profile >= PROFILE_LEVEL_TRACE_1) {
    LOG_TOPIC("36a75", INFO, Logger::QUERIES) << elapsedSince(_startTime)
                                              << " Query::~Query queryString: "
                                              << " this: " << (uintptr_t)this;
  }

  _profile.reset(); // unregister from QueryList

  // this will reset _trx, so _trx is invalid after here
  try {
    ExecutionState state = cleanupPlanAndEngine(TRI_ERROR_INTERNAL, /*sync*/true);
    TRI_ASSERT(state != ExecutionState::WAITING);
  } catch (...) {
    // unfortunately we cannot do anything here, as we are in 
    // a destructor
  }
  
  unregisterSnippets();

  exitV8Context();

  _snippets.clear(); // simon: must be before plan
  _plans.clear(); // simon: must be before AST
  _ast.reset();

  LOG_TOPIC("f5cee", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::~Query this: " << (uintptr_t)this;
}

bool Query::killed() const {
  if (_queryOptions.maxRuntime > std::numeric_limits<double>::epsilon() &&
      elapsedSince(_startTime) > _queryOptions.maxRuntime) {
    return true;
  }
  return _killState.load(std::memory_order_relaxed) == KillState::Killed;
}

/// @brief set the query to killed
void Query::kill() {
  KillState exp = KillState::None;
  if (!_killState.compare_exchange_strong(exp, KillState::Killed, std::memory_order_release)) {
    return;
  }
  
  for (auto& pair : _snippets) {
    ExecutionEngine* engine = pair.second.get();
    if (engine != nullptr) {
      // killing is best effort...
      // intentionally ignoring the result of this call here
      engine->kill();
    }
  }
}
  
/// @brief return the start time of the query (steady clock value)
double Query::startTime() const noexcept { return _startTime; }

void Query::prepareQuery(SerializationFormat format) {
  init(/*createProfile*/ true);
  enterState(QueryExecutionState::ValueType::PARSING);

  std::unique_ptr<ExecutionPlan> plan = preparePlan();
  TRI_ASSERT(plan != nullptr);
  plan->findVarUsage();

  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(_trx->status() == transaction::Status::RUNNING);

  // note that the engine returned here may already be present in our
  // own _engine attribute (the instanciation procedure may modify us
  // by calling our engine(ExecutionEngine*) function
  // this is confusing and should be fixed!
  ExecutionEngine::instantiateFromPlan(*this, *plan, /*planRegisters*/!_queryString.empty(),
                                       format, _snippets);

  _plans.push_back(std::move(plan));

  if (_snippets.size() > 1) {  // register coordinator snippets
    TRI_ASSERT(_trx->state()->isCoordinator());
    QueryRegistry* registry = QueryRegistryFeature::registry();
    if (!registry) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SHUTTING_DOWN, "query registry not available");
    }
    registry->registerSnippets(_snippets);
  }
  
  if (_profile) {
    _profile->registerInQueryList();
  }

  enterState(QueryExecutionState::ValueType::EXECUTION);
}

/// @brief prepare an AQL query, this is a preparation for execute, but
/// execute calls it internally. The purpose of this separate method is
/// to be able to only prepare a query from VelocyPack and then store it in the
/// QueryRegistry.
std::unique_ptr<ExecutionPlan> Query::preparePlan() {
  TRI_ASSERT(!_queryString.empty());
  LOG_TOPIC("9625e", DEBUG, Logger::QUERIES) << elapsedSince(_startTime)
                                             << " Query::prepare"
                                             << " this: " << (uintptr_t)this;

  TRI_ASSERT(_ast != nullptr);
  Parser parser(*this, *_ast, _queryString);
  parser.parse();

  // put in bind parameters
  parser.ast()->injectBindParameters(_bindParameters, this->resolver());

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
  // create the transaction object, but do not start it yet
  _trx->addHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel

  // As soon as we start to instantiate the plan we have to clean it
  // up before killing the unique_ptr

  // we have an AST, optimize the ast
  enterState(QueryExecutionState::ValueType::AST_OPTIMIZATION);

  _ast->validateAndOptimize(*_trx);

  enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);

  Result res = _trx->begin();

  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }
  TRI_ASSERT(_trx->status() == transaction::Status::RUNNING);

  enterState(QueryExecutionState::ValueType::PLAN_INSTANTIATION);

  auto plan = ExecutionPlan::instantiateFromAst(_ast.get());

  // Run the query optimizer:
  enterState(QueryExecutionState::ValueType::PLAN_OPTIMIZATION);
  arangodb::aql::Optimizer opt(_queryOptions.maxNumberOfPlans);
  // get enabled/disabled rules
  opt.createPlans(std::move(plan), _queryOptions, false);
  // Now plan and all derived plans belong to the optimizer
  plan = opt.stealBest();  // Now we own the best one again

  TRI_ASSERT(plan != nullptr);

  // return the V8 context if we are in one
  exitV8Context();

  return plan;
}

/// @brief execute an AQL query
ExecutionState Query::execute(QueryResult& queryResult) {
  LOG_TOPIC("e8ed7", DEBUG, Logger::QUERIES) << elapsedSince(_startTime)
                                             << " Query::execute"
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
          auto cacheEntry =
              arangodb::aql::QueryCache::instance()->lookup(&_vocbase, hash(), _queryString,
                                                            bindParameters());

          if (cacheEntry != nullptr) {
            bool hasPermissions = true;
            ExecContext const& exec = ExecContext::current();

            // got a result from the query cache
            if (!exec.isSuperuser()) {
              for (auto& dataSource : cacheEntry->_dataSources) {
                auto const& dataSourceName = dataSource.second;

                if (!exec.canUseCollection(dataSourceName, auth::Level::RO)) {
                  // cannot use query cache result because of permissions
                  hasPermissions = false;
                  break;
                }
              }
            }

            if (hasPermissions) {
              // we don't have yet a transaction when we're here, so let's
              // create a mimimal context to build the result
              queryResult.context = transaction::StandaloneContext::Create(_vocbase);
              TRI_ASSERT(cacheEntry->_queryResult != nullptr);
              queryResult.data = cacheEntry->_queryResult;
              queryResult.extra = cacheEntry->_stats;
              queryResult.cached = true;

              return ExecutionState::DONE;
            }
            // if no permissions, fall through to regular querying
          }
        }

        // will throw if it fails
        if (!_ast) { // simon: hack for AQL_EXECUTEJSON
          prepareQuery(SerializationFormat::SHADOWROWS);
        }

        log();

        _resultBuilderOptions = std::make_unique<VPackOptions>(VPackOptions::Defaults);
        _resultBuilderOptions->buildUnindexedArrays = true;
        _resultBuilderOptions->buildUnindexedObjects = true;

        // NOTE: If the options have a shorter lifetime than the builder, it
        // gets invalid (at least set() and close() are broken).
        _resultBuilder = std::make_shared<VPackBuilder>(_resultBuilderOptions.get());

        // reserve some space in Builder to avoid frequent reallocs
        _resultBuilder->reserve(16 * 1024);

        _resultBuilder->openArray();
        _executionPhase = ExecutionPhase::EXECUTE;
      }
      [[fallthrough]];
      case ExecutionPhase::EXECUTE: {
        TRI_ASSERT(_resultBuilder != nullptr);
        TRI_ASSERT(_resultBuilder->isOpenArray());
        TRI_ASSERT(_trx != nullptr);

        if (useQueryCache && (isModificationQuery()  || !_warnings.empty() ||
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
          auto const& [state, skipped, block] = engine->execute(AqlCallStack::emptyStack(), AqlCallList{AqlCall{}});
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

          if (!_queryOptions.silent) {
            // cache low-level pointer to avoid repeated shared-ptr-derefs
            TRI_ASSERT(_resultBuilder != nullptr);
            auto& resultBuilder = *_resultBuilder;

            size_t const n = block->size();

            for (size_t i = 0; i < n; ++i) {
              AqlValue const& val = block->getValueReference(i, resultRegister);

              if (!val.isEmpty()) {
                val.toVelocyPack(&vpackOptions(), resultBuilder, useQueryCache);
              }
            }
          }

          if (state == ExecutionState::DONE) {
            break;
          }
        }

        // must close result array here because it must be passed as a closed
        // array to the query cache
        _resultBuilder->close();

        if (useQueryCache && _warnings.empty()) {
          std::unordered_map<std::string, std::string> dataSources = _queryDataSources;

          _trx->state()->allCollections(  // collect transaction DataSources
              [&dataSources](TransactionCollection& trxCollection) -> bool {
                auto const& c = trxCollection.collection();
                dataSources.try_emplace(c->guid(), c->name());
                return true;  // continue traversal
              });

          // create a query cache entry for later storage
          _cacheEntry =
              std::make_unique<QueryCacheResultEntry>(hash(), _queryString, _resultBuilder,
                                                      bindParameters(),
                                                      std::move(dataSources)  // query DataSources
              );
        }

        queryResult.data = std::move(_resultBuilder);
        queryResult.context = _trx->transactionContext();
        _executionPhase = ExecutionPhase::FINALIZE;
      }

      [[fallthrough]];
      case ExecutionPhase::FINALIZE: {
        // will set warnings, stats, profile and cleanup plan and engine
        return finalize(queryResult);
      }
    }
    // We should not be able to get here
    TRI_ASSERT(false);
  } catch (arangodb::basics::Exception const& ex) {
    queryResult.reset(Result(ex.code(), "AQL: " + ex.message() +
                                            QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(ex.code(), /*sync*/true);
  } catch (std::bad_alloc const&) {
    queryResult.reset(Result(TRI_ERROR_OUT_OF_MEMORY,
                             TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY) +
                                 QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_OUT_OF_MEMORY, /*sync*/true);
  } catch (std::exception const& ex) {
    queryResult.reset(Result(TRI_ERROR_INTERNAL,
                             ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_INTERNAL, /*sync*/true);
  } catch (...) {
    queryResult.reset(Result(TRI_ERROR_INTERNAL,
                             TRI_errno_string(TRI_ERROR_INTERNAL) +
                                 QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_INTERNAL, /*sync*/true);
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
  std::shared_ptr<SharedQueryState> ss;

  QueryResult queryResult;
  do {
    auto state = execute(queryResult);
    if (state != aql::ExecutionState::WAITING) {
      TRI_ASSERT(state == aql::ExecutionState::DONE);
      return queryResult;
    }

    if (!ss) {
      ss = sharedState();
    }

    TRI_ASSERT(ss != nullptr);
    ss->waitForAsyncWakeup();
  } while (true);
}

// execute an AQL query: may only be called with an active V8 handle scope
QueryResultV8 Query::executeV8(v8::Isolate* isolate) {
  LOG_TOPIC("6cac7", DEBUG, Logger::QUERIES) << elapsedSince(_startTime)
                                             << " Query::executeV8"
                                             << " this: " << (uintptr_t)this;

  aql::QueryResultV8 queryResult;

  try {
    bool useQueryCache = canUseQueryCache();

    if (useQueryCache) {
      // check the query cache for an existing result
      auto cacheEntry =
          arangodb::aql::QueryCache::instance()->lookup(&_vocbase, hash(), _queryString,
                                                        bindParameters());

      if (cacheEntry != nullptr) {
        bool hasPermissions = true;
        auto ctx = transaction::StandaloneContext::Create(_vocbase);
        ExecContext const& exec = ExecContext::current();

        // got a result from the query cache
        if (!exec.isSuperuser()) {
          for (auto const& dataSource : cacheEntry->_dataSources) {
            auto const& dataSourceName = dataSource.second;

            if (!exec.canUseCollection(dataSourceName, auth::Level::RO)) {
              // cannot use query cache result because of permissions
              hasPermissions = false;
              break;
            }
          }
        }

        if (hasPermissions) {
          // we don't have yet a transaction when we're here, so let's create
          // a mimimal context to build the result
          queryResult.context = ctx;
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
    prepareQuery(SerializationFormat::SHADOWROWS);

    log();

    if (useQueryCache && (isModificationQuery()  || !_warnings.empty() ||
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
      uint32_t j = 0;
      ExecutionState state = ExecutionState::HASMORE;
      auto context = TRI_IGETC;
      while (state != ExecutionState::DONE) {
        auto res = engine->getSome(ExecutionBlock::DefaultBatchSize);
        state = res.first;
        while (state == ExecutionState::WAITING) {
          ss->waitForAsyncWakeup();
          res = engine->getSome(ExecutionBlock::DefaultBatchSize);
          state = res.first;
        }
        SharedAqlItemBlockPtr value = std::move(res.second);

        // value == nullptr => state == DONE
        TRI_ASSERT(value != nullptr || state == ExecutionState::DONE);

        if (value == nullptr) {
          continue;
        }

        if (!_queryOptions.silent) {
          size_t const n = value->size();

          for (size_t i = 0; i < n; ++i) {
            AqlValue const& val = value->getValueReference(i, resultRegister);

            if (!val.isEmpty()) {
              VPackOptions const* options = _trx->transactionContext()->getVPackOptions();
              resArray->Set(context, j++, val.toV8(isolate, options)).FromMaybe(false);

              if (useQueryCache) {
                val.toVelocyPack(&vpackOptions(), *builder, true);
              }

              if (V8PlatformFeature::isOutOfMemory(isolate)) {
                THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
              }
            }
          }
        }

        if (killed()) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
        }
      }

      builder->close();
    } catch (...) {
      LOG_TOPIC("8a6bf", DEBUG, Logger::QUERIES)
          << elapsedSince(_startTime)
          << " got an exception executing "
          << " this: " << (uintptr_t)this;
      throw;
    }

    queryResult.v8Data = resArray;
    queryResult.context = _trx->transactionContext();

    if (useQueryCache && _warnings.empty()) {
      auto dataSources = _queryDataSources;

      _trx->state()->allCollections(  // collect transaction DataSources
          [&dataSources](TransactionCollection& trxCollection) -> bool {
            auto const& c = trxCollection.collection();
            dataSources.try_emplace(c->guid(), c->name());
            return true;  // continue traversal
          });

      // create a cache entry for later usage
      _cacheEntry = std::make_unique<QueryCacheResultEntry>(hash(), _queryString,
                                                            builder, bindParameters(),
                                                            std::move(dataSources)  // query DataSources
      );
    }

    ss->resetWakeupHandler();
    // will set warnings, stats, profile and cleanup plan and engine
    ExecutionState state = finalize(queryResult);
    while (state == ExecutionState::WAITING) {
      ss->waitForAsyncWakeup();
      state = finalize(queryResult);
    }
    // fallthrough to returning queryResult below...
  } catch (arangodb::basics::Exception const& ex) {
    queryResult.reset(Result(ex.code(), "AQL: " + ex.message() +
                                            QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(ex.code(), /*sync*/true);
  } catch (std::bad_alloc const&) {
    queryResult.reset(Result(TRI_ERROR_OUT_OF_MEMORY,
                             TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY) +
                                 QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_OUT_OF_MEMORY, /*sync*/true);
  } catch (std::exception const& ex) {
    queryResult.reset(Result(TRI_ERROR_INTERNAL,
                             ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_INTERNAL, /*sync*/true);
  } catch (...) {
    queryResult.reset(Result(TRI_ERROR_INTERNAL,
                             TRI_errno_string(TRI_ERROR_INTERNAL) +
                                 QueryExecutionState::toStringWithPrefix(_execState)));
    cleanupPlanAndEngine(TRI_ERROR_INTERNAL, /*sync*/true);
  }

  return queryResult;
}

ExecutionState Query::finalize(QueryResult& result) {
  if (result.extra == nullptr || !result.extra->isOpenObject()) {
    // The above condition is not true if we have already waited.
    LOG_TOPIC("fc22c", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " Query::finalize: before _trx->commit"
        << " this: " << (uintptr_t)this;

    Result commitResult = _trx->commit();
    if (commitResult.fail()) {
      THROW_ARANGO_EXCEPTION(commitResult);
    }

    LOG_TOPIC("7ef18", DEBUG, Logger::QUERIES)
        << elapsedSince(_startTime)
        << " Query::finalize: before cleanupPlanAndEngine"
        << " this: " << (uintptr_t)this;

    enterState(QueryExecutionState::ValueType::FINALIZATION);

    result.extra = std::make_shared<VPackBuilder>();
    result.extra->openObject(true);
  }

  const bool addPlan = _queryOptions.profile >= PROFILE_LEVEL_BLOCKS &&
                       ServerState::instance()->isSingleServerOrCoordinator();
  auto state = cleanupPlanAndEngine(TRI_ERROR_NO_ERROR, /*sync*/false, result.extra.get(), addPlan);
  if (state == ExecutionState::WAITING) {
    return state;
  }

  _warnings.toVelocyPack(*result.extra);

  double now = currentSteadyClockValue();
  if (_profile != nullptr && _queryOptions.profile >= PROFILE_LEVEL_BASIC) {
    _profile->setStateEnd(QueryExecutionState::ValueType::FINALIZATION, now);
    _profile->toVelocyPack(*(result.extra));
  }
  result.extra->close();

  if (_cacheEntry != nullptr) {
    _cacheEntry->_stats = result.extra;
    QueryCache::instance()->store(&_vocbase, std::move(_cacheEntry));
  }

  // patch executionTime stats value in place
  // we do this because "executionTime" should include the whole span of the
  // execution and we have to set it at the very end
  double const rt = now - _startTime;
  try {
    basics::VelocyPackHelper::patchDouble(result.extra->slice().get("stats").get("executionTime"), rt);
  } catch (...) {
    // if the query has failed, the slice may not
    // contain a proper "stats" object once we get here.
  }

  LOG_TOPIC("95996", DEBUG, Logger::QUERIES) << rt 
                                             << " Query::finalize:returning"
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

  } catch (arangodb::basics::Exception const& ex) {
    result.reset(Result(ex.code(), ex.message()));
  } catch (std::bad_alloc const&) {
    result.reset(Result(TRI_ERROR_OUT_OF_MEMORY));
  } catch (std::exception const& ex) {
    result.reset(Result(TRI_ERROR_INTERNAL, ex.what()));
  } catch (...) {
    result.reset(Result(TRI_ERROR_INTERNAL, "an unknown error occurred while parsing the query"));
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
    parser.ast()->injectBindParameters(_bindParameters, this->resolver());

    // optimize and validate the ast
    enterState(QueryExecutionState::ValueType::AST_OPTIMIZATION);

    // create the transaction object, but do not start it yet
    _trx = AqlTransaction::create(_transactionContext, _collections,
                                  _queryOptions.transactionOptions);

    // we have an AST
    Result res = _trx->begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    enterState(QueryExecutionState::ValueType::LOADING_COLLECTIONS);
    _ast->validateAndOptimize(*_trx);

    enterState(QueryExecutionState::ValueType::PLAN_INSTANTIATION);
    std::unique_ptr<ExecutionPlan> plan =
        ExecutionPlan::instantiateFromAst(parser.ast());

    // Run the query optimizer:
    enterState(QueryExecutionState::ValueType::PLAN_OPTIMIZATION);
    arangodb::aql::Optimizer opt(_queryOptions.maxNumberOfPlans);
    // get enabled/disabled rules
    opt.createPlans(std::move(plan), _queryOptions, true);

    enterState(QueryExecutionState::ValueType::FINALIZATION);

    auto preparePlanForSerialization = [&](std::unique_ptr<ExecutionPlan> const& plan) {
      plan->findVarUsage();
      plan->planRegisters(_queryOptions.explainRegisters);
      plan->findCollectionAccessVariables();
      plan->prepareTraversalOptions();
    };

    if (_queryOptions.allPlans) {
      result.data = std::make_shared<VPackBuilder>();
      {
        VPackArrayBuilder guard(result.data.get());

        auto const& plans = opt.getPlans();
        for (auto& it : plans) {
          auto& pln = it.first;
          TRI_ASSERT(pln != nullptr);

          preparePlanForSerialization(pln);
          pln->toVelocyPack(*result.data.get(), parser.ast(), _queryOptions.verbosePlans,
                            _queryOptions.explainRegisters);
        }
      }
      // cacheability not available here
      result.cached = false;
    } else {
      // Now plan and all derived plans belong to the optimizer
      std::unique_ptr<ExecutionPlan> bestPlan = opt.stealBest();  // Now we own the best one again
      TRI_ASSERT(bestPlan != nullptr);

      preparePlanForSerialization(bestPlan);
      result.data = bestPlan->toVelocyPack(parser.ast(), _queryOptions.verbosePlans,
                                           _queryOptions.explainRegisters);

      // cacheability
      result.cached = (!_queryString.empty() && !isModificationQuery() &&
                       _warnings.empty() && _ast->root()->isCacheable());
    }

    // technically no need to commit, as we are only explaining here
    auto commitResult = _trx->commit();
    if (commitResult.fail()) {
      THROW_ARANGO_EXCEPTION(commitResult);
    }

    result.extra = std::make_shared<VPackBuilder>();
    {

      VPackObjectBuilder guard(result.extra.get(), true);
      _warnings.toVelocyPack(*result.extra);
      result.extra->add(VPackValue("stats"));
      opt._stats.toVelocyPack(*result.extra);
    }

  } catch (arangodb::basics::Exception const& ex) {
    result.reset(Result(ex.code(), ex.message() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (std::bad_alloc const&) {
    result.reset(Result(TRI_ERROR_OUT_OF_MEMORY,
                        TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY) +
                        QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (std::exception const& ex) {
    result.reset(Result(TRI_ERROR_INTERNAL, ex.what() + QueryExecutionState::toStringWithPrefix(_execState)));
  } catch (...) {
    result.reset(Result(TRI_ERROR_INTERNAL, TRI_errno_string(TRI_ERROR_INTERNAL) + QueryExecutionState::toStringWithPrefix(_execState)));
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

/// @brief enter a V8 context
void Query::enterV8Context() {
  if (!_contextOwnedByExterior) {
    if (_v8Context == nullptr) {
      if (V8DealerFeature::DEALER == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "V8 engine is disabled");
      }
      JavaScriptSecurityContext securityContext =
          JavaScriptSecurityContext::createQueryContext();
      TRI_ASSERT(V8DealerFeature::DEALER != nullptr);
      _v8Context = V8DealerFeature::DEALER->enterContext(&_vocbase, securityContext);

      if (_v8Context == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_RESOURCE_LIMIT,
            "unable to enter V8 context for query execution");
      }

      // register transaction in context
      TRI_ASSERT(_trx != nullptr);

      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT));
      auto ctx = static_cast<arangodb::transaction::V8Context*>(v8g->_transactionContext);
      if (ctx != nullptr) {
        ctx->enterV8Context(_trx->stateShrdPtr());
      }
    }

    TRI_ASSERT(_v8Context != nullptr);
  }
}

/// @brief return a V8 context
void Query::exitV8Context() {
  if (!_contextOwnedByExterior) {
    if (_v8Context != nullptr) {
      // unregister transaction in context
      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT));
      auto ctx = static_cast<arangodb::transaction::V8Context*>(v8g->_transactionContext);
      if (ctx != nullptr) {
        ctx->exitV8Context();
      }

      TRI_ASSERT(V8DealerFeature::DEALER != nullptr);
      V8DealerFeature::DEALER->exitContext(_v8Context);
      _v8Context = nullptr;
    }
  }
}

/// @brief initializes the query
void Query::init(bool createProfile) {
  TRI_ASSERT(!_profile && !_ast);
  if (_profile || _ast) {
    // already called
    return;
  }

  TRI_ASSERT(_profile == nullptr);
  // adds query to QueryList which is needed for /_api/query/current
  if (createProfile && !ServerState::instance()->isDBServer()) {
    _profile = std::make_unique<QueryProfile>(this);
  }
  enterState(QueryExecutionState::ValueType::INITIALIZATION);

  TRI_ASSERT(_ast == nullptr);
  _ast = std::make_unique<Ast>(*this);
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
void Query::log() {
  if (!_queryString.empty()) {
    LOG_TOPIC("8a86a", TRACE, Logger::QUERIES)
        << "executing query " << _queryId << ": '" << _queryString.extract(1024) << "'";
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
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("fullcount:true"), hashval);
  } else {
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("fullcount:false"), hashval);
  }

  // handle "count" option
  if (_queryOptions.count) {
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("count:true"), hashval);
  } else {
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("count:false"), hashval);
  }

  // handle "optimizer.inspectSimplePlans" option
  if (_queryOptions.inspectSimplePlans) {
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("inspect:true"), hashval);
  } else {
    hashval = fasthash64(TRI_CHAR_LENGTH_PAIR("inspect:false"), hashval);
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
  if (_queryString.size() < 8 || _queryOptions.silent) {
    return false;
  }

  auto queryCacheMode = QueryCache::instance()->mode();

  if (_queryOptions.cache &&
      (queryCacheMode == CACHE_ALWAYS_ON || queryCacheMode == CACHE_ON_DEMAND)) {
    // cache mode is set to always on or on-demand...
    // query will only be cached if `cache` attribute is not set to false

    // cannot use query cache on a coordinator at the moment
    return !arangodb::ServerState::instance()->isRunningInCluster();
  }

  return false;
}

/// @brief enter a new state
void Query::enterState(QueryExecutionState::ValueType state) {
  LOG_TOPIC("d8767", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " Query::enterState: " << state << " this: " << (uintptr_t)this;
  if (_profile != nullptr) {
    // record timing for previous state
    _profile->setStateDone(_execState);
  }

  // and adjust the state
  _execState = state;
}

/// @brief cleanup plan and engine for current query
ExecutionState Query::cleanupPlanAndEngine(int errorCode, bool sync,
                                           VPackBuilder* statsBuilder,
                                           bool includePlan) {
  KillState exp = _killState.load(std::memory_order_acquire);
  if (exp == KillState::Killed) {
    return ExecutionState::DONE;
  } else if (exp == KillState::None) {
    if (!_killState.compare_exchange_strong(exp, KillState::Shutdown, std::memory_order_release)) {
      TRI_ASSERT(exp == KillState::Killed);
      return ExecutionState::DONE;
    }
    // the following call removes the query from the list of currently
    // running queries. so whoever fetches that list will not see a Query that
    // is about to shut down/be destroyed
    if (_profile != nullptr) {
      _profile->unregisterFromQueryList();
    }
  } else {
    TRI_ASSERT(exp == KillState::Shutdown);
  }
  
  auto* engine = rootEngine();
  if (engine) {
    std::shared_ptr<SharedQueryState> ss;
    if (sync) {
      ss = engine->sharedState();
      ss->resetWakeupHandler();
    }
    try {
      while (true) {
        auto [state, res] = engine->shutdown(errorCode);
        if (state != ExecutionState::WAITING) {
          break;
        } else if (!sync) {
          return state;
        }
        ss->waitForAsyncWakeup();
      }
      engine->sharedState()->invalidate();
    } catch (...) {
      // shutdown may fail but we must not throw here
      // (we're also called from the destructor)
    }
  }

  if (statsBuilder != nullptr && !_snippets.empty()) {
    TRI_ASSERT(statsBuilder->isOpenObject());
    ExecutionStats stats;
    stats.requests += _numRequests.load(std::memory_order_relaxed);
    stats.setPeakMemoryUsage(_resourceMonitor.peakMemoryUsage());
    stats.setExecutionTime(elapsedSince(_startTime));
    for (auto& [eId, engine] : _snippets) {
      engine->collectExecutionStats(stats);
      engine->setShutdown(ExecutionEngine::ShutdownState::Done);
    }

    statsBuilder->add(VPackValue("stats"));
    stats.toVelocyPack(*statsBuilder, _queryOptions.fullCount);

    if (includePlan) {
      TRI_ASSERT(_plans.size() == 1);
      auto& plan = _plans[0];

      if (ServerState::instance()->isCoordinator()) {
        std::initializer_list<arangodb::aql::ExecutionNode::NodeType> const collectionNodeTypes{
            arangodb::aql::ExecutionNode::ENUMERATE_COLLECTION,
            arangodb::aql::ExecutionNode::INDEX,
            arangodb::aql::ExecutionNode::REMOVE,
            arangodb::aql::ExecutionNode::INSERT,
            arangodb::aql::ExecutionNode::UPDATE,
            arangodb::aql::ExecutionNode::REPLACE,
            arangodb::aql::ExecutionNode::UPSERT};

        ::arangodb::containers::SmallVector<ExecutionNode*>::allocator_type::arena_type a;
        ::arangodb::containers::SmallVector<ExecutionNode*> nodes{a};
        plan->findNodesOfType(nodes, collectionNodeTypes, true);

        for (ExecutionNode* n : nodes) {
          // clear shards so we get back the full collection name when
          // serializing the plan
          auto cn = dynamic_cast<CollectionAccessingNode*>(n);
          if (cn) {
            cn->setUsedShard("");
          }
        }
      }

      // remove additional ASYNC and TraversalNodes added for traversal parallelization
      ::arangodb::containers::SmallVector<ExecutionNode*>::allocator_type::arena_type a;
      ::arangodb::containers::SmallVector<ExecutionNode*> nodes{a};
      plan->findUniqueNodesOfType(nodes, {arangodb::aql::ExecutionNode::MUTEX}, true);

      for (ExecutionNode* n : nodes) {
        auto parents = n->getParents();
        for (size_t i = 0; i < parents.size(); i++) {
          TRI_ASSERT(parents[i]->getType() == ExecutionNode::DISTRIBUTE_CONSUMER);
          ExecutionNode* graph = parents[i]->getFirstParent();
          if (graph) {
            TRI_ASSERT(graph->getType() == ExecutionNode::TRAVERSAL);
            plan->unlinkNode(parents[i]); // Dist-Consumer node does not like toVelocyPack
            if (i > 0) {
              // unlink additional ASYNC nodes, explainer does not handle cycles
              ExecutionNode* async = graph->getFirstParent();
              if (async) {
                TRI_ASSERT(async->getType() == ExecutionNode::ASYNC);
                ExecutionNode* gather = async->getFirstParent();
                if (gather) {
                  TRI_ASSERT(gather->getType() == ExecutionNode::GATHER);
                  gather->removeDependency(async);
                  plan->clearVarUsageComputed();
                }
              }
            }
          }
        }
      }
      plan->findVarUsage();

      statsBuilder->add(VPackValue("plan"));
      plan->toVelocyPack(*statsBuilder, _ast.get(), false, ExplainRegisterPlan::No);
      // needed to happen before plan cleanup
    }
  }

  return ExecutionState::DONE;
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

  if (_ast->canApplyParallelism()) {
    TRI_ASSERT(!_ast->containsModificationNode());
    return _transactionContext->clone();
  }
  return _transactionContext;
}

velocypack::Options const& Query::vpackOptions() const {
  return *(_transactionContext->getVPackOptions());
}

transaction::Methods& Query::trxForOptimization() {
  TRI_ASSERT(_execState != QueryExecutionState::ValueType::EXECUTION);
  return *_trx;
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
void Query::initForTests() {
  this->init(/*createProfile*/ false);
  _trx = AqlTransaction::create(_transactionContext, _collections,
                                _queryOptions.transactionOptions,
                                std::unordered_set<std::string>{});
  // create the transaction object, but do not start it yet
  _trx->addHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel
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
    TRI_ASSERT(ServerState::instance()->isDBServer() || _snippets[0].first == 0);
    return _snippets[0].second.get();
  }
  return nullptr;
}

ClusterQuery::ClusterQuery(std::shared_ptr<transaction::Context> const& ctx,
                           QueryOptions&& options)
    : Query(ctx, aql::QueryString(), /*bindParams*/ nullptr, std::move(options),
            /*sharedState*/ ServerState::instance()->isDBServer()
                ? nullptr
                : std::make_shared<SharedQueryState>(ctx->vocbase().server())) {}

ClusterQuery::~ClusterQuery() {
  try {
    _traversers.clear();
  } catch (...) {
  }
}

void ClusterQuery::prepareClusterQuery(SerializationFormat format,
                                       VPackSlice querySlice,
                                       VPackSlice collections,
                                       VPackSlice variables,
                                       VPackSlice snippets,
                                       VPackSlice traverserSlice,
                                       VPackBuilder& answerBuilder,
                                       arangodb::QueryAnalyzerRevisions const& analyzersRevision) {
  LOG_TOPIC("9636f", DEBUG, Logger::QUERIES) << elapsedSince(_startTime)
                                             << " ClusterQuery::prepareClusterQuery"
                                             << " this: " << (uintptr_t)this;

  init(/*createProfile*/ true);

  VPackSlice val = querySlice.get("isModificationQuery");
  if (val.isBool() && val.getBool()) {
    _ast->setContainsModificationNode();
  }
  val = querySlice.get("isAsyncQuery");
  if (val.isBool() && val.getBool()) {
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
  // create the transaction object, but do not start it yet
  _trx->addHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL);  // only used on toplevel
  if (_trx->state()->isDBServer()) {
    _trx->state()->acceptAnalyzersRevision(analyzersRevision);
  }
  Result res = _trx->begin();
  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  enterState(QueryExecutionState::ValueType::PARSING);

  auto instantiateSnippet = [&](VPackSlice snippet) {
    auto plan = ExecutionPlan::instantiateFromVelocyPack(_ast.get(), snippet);
    TRI_ASSERT(plan != nullptr);

    plan->findVarUsage();

    ExecutionEngine::instantiateFromPlan(*this, *plan, /*planRegisters*/!_queryString.empty(),
                                         format, _snippets);
    _plans.push_back(std::move(plan));
  };

  answerBuilder.add("snippets", VPackValue(VPackValueType::Object));
  for (auto pair : VPackObjectIterator(snippets, /*sequential*/true)) {
    instantiateSnippet(pair.value);

    TRI_ASSERT(!_snippets.empty());

    QueryId qId = _snippets.back().first;
    if (_trx->state()->isDBServer()) {
      qId = TRI_NewTickServer();
      _snippets.back().first = qId;
    }
    TRI_ASSERT(!_trx->state()->isDBServer() || qId != 0);

    answerBuilder.add(pair.key);
    answerBuilder.add(VPackValue(arangodb::basics::StringUtils::itoa(qId)));
  }
  answerBuilder.close(); // snippets

  if (!_snippets.empty()) {
    TRI_ASSERT(_trx->state()->isDBServer() || _snippets[0].first == 0);
    // simon: for AQL_EXECUTEJSON
    if (_trx->state()->isCoordinator()) {  // register coordinator snippets
      TRI_ASSERT(_trx->state()->isCoordinator());
      QueryRegistryFeature::registry()->registerSnippets(_snippets);
    }
  }

  if (traverserSlice.isArray()) {
    // used to be RestAqlHandler::registerTraverserEngines
    answerBuilder.add("traverserEngines", VPackValue(VPackValueType::Array));
    for (auto const& te : VPackArrayIterator(traverserSlice)) {

      aql::EngineId eId = TRI_NewTickServer();
      auto engine = traverser::BaseEngine::BuildEngine(_vocbase, *this, te);

      _traversers.emplace_back(eId, std::move(engine));

      answerBuilder.add(VPackValue(eId));
    }
    answerBuilder.close();  // traverserEngines
  }
  TRI_ASSERT(_trx != nullptr);
  
  if (_profile) {
    _profile->registerInQueryList();
  }
  enterState(QueryExecutionState::ValueType::EXECUTION);
}

Result ClusterQuery::finalizeClusterQuery(ExecutionStats& stats, int errorCode) {
  TRI_ASSERT(_trx);
  TRI_ASSERT(ServerState::instance()->isDBServer());

  LOG_TOPIC("fc33c", DEBUG, Logger::QUERIES)
       << elapsedSince(_startTime)
       << " Query::finalizeSnippets: before _trx->commit, errorCode: "
       << errorCode << ", this: " << (uintptr_t)this;
  
  for (auto& [eId, engine] : _snippets) { // simon: no need to pass through shutdown
    engine->setShutdown(ExecutionEngine::ShutdownState::Done);

    engine->sharedState()->invalidate();
    engine->collectExecutionStats(stats);
  }

  Result finishResult;
  if (_trx->status() == transaction::Status::RUNNING) {
    if (errorCode == TRI_ERROR_NO_ERROR) {
      // no error. we need to commit the transaction
      finishResult = _trx->commit();
    } else {
      // got an error. we need to abort the transaction
      finishResult = _trx->abort();
    }
  }

  LOG_TOPIC("8ea28", DEBUG, Logger::QUERIES)
       << elapsedSince(_startTime)
       << " Query::finalizeSnippets: before cleanupPlanAndEngine"
       << " this: " << (uintptr_t)this;

  enterState(QueryExecutionState::ValueType::FINALIZATION);

  stats.requests += _numRequests.load(std::memory_order_relaxed);
  stats.setPeakMemoryUsage(_resourceMonitor.peakMemoryUsage());
  stats.setExecutionTime(elapsedSince(_startTime));
  
  _snippets.clear();
  _traversers.clear();
  LOG_TOPIC("5fde0", DEBUG, Logger::QUERIES)
      << elapsedSince(_startTime)
      << " ClusterQuery::finalizeClusterQuery: done"
      << " this: " << (uintptr_t)this;
  return finishResult;
}
