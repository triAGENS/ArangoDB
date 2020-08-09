////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_AQL_EXECUTION_ENGINE_H
#define ARANGOD_AQL_EXECUTION_ENGINE_H 1

#include "Aql/ExecutionState.h"
#include "Aql/ExecutionStats.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/types.h"
#include "Basics/Common.h"
#include "Containers/SmallVector.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arangodb {
namespace aql {

class AqlCallList;
class AqlCallStack;
class AqlItemBlock;
class AqlItemBlockManager;
class ExecutionBlock;
class ExecutionNode;
class ExecutionPlan;
class Query;
class QueryContext;
class QueryRegistry;
class SkipResult;
class SharedQueryState;

class ExecutionEngine {
 public:
  /// @brief create the engine
  ExecutionEngine(QueryContext& query,
                  AqlItemBlockManager& itemBlockManager,
                  SerializationFormat format,
                  std::shared_ptr<SharedQueryState> sharedState = nullptr);

  /// @brief destroy the engine, frees all assigned blocks
  TEST_VIRTUAL ~ExecutionEngine();

 public:
  
  // @brief create an execution engine from a plan
  static void instantiateFromPlan(Query& query,
                                  ExecutionPlan& plan,
                                  bool planRegisters,
                                  SerializationFormat format,
                                  SnippetList& list);
  
  TEST_VIRTUAL Result createBlocks(std::vector<ExecutionNode*> const& nodes,
                                   MapRemoteToSnippet const& queryIds);

  /// @brief get the root block
  TEST_VIRTUAL ExecutionBlock* root() const;

  /// @brief set the root block
  TEST_VIRTUAL void root(ExecutionBlock* root);

  /// @brief get the query
  QueryContext& getQuery() const;
  
  std::shared_ptr<SharedQueryState> sharedState() const {
    return _sharedState;
  }

  /// @brief server to snippet mapping
  void snippetMapping(MapRemoteToSnippet&& dbServerMapping,
                      std::map<std::string, QueryId>&& serverToQueryId) {
    _dbServerMapping = std::move(dbServerMapping);
    _serverToQueryId = std::move(serverToQueryId);
  }
  
  /// @brief kill the query
  void kill();

  /// @brief initializeCursor, could be called multiple times
  std::pair<ExecutionState, Result> initializeCursor(SharedAqlItemBlockPtr&& items, size_t pos);

  /// @brief shutdown, will be called exactly once for the whole query, blocking
  /// variant
  Result shutdownSync(int errorCode) noexcept;

  /// @brief shutdown, will be called exactly once for the whole query, may
  /// return waiting
  std::pair<ExecutionState, Result> shutdown(int errorCode);

  auto execute(AqlCallStack const& stack, AqlCallList clientCall)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  auto executeForClient(AqlCallStack const& stack, AqlCallList clientCall,
                        std::string const& clientId)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  /// @brief getSome
  std::pair<ExecutionState, SharedAqlItemBlockPtr> getSome(size_t atMost);

  /// @brief skipSome
  std::pair<ExecutionState, size_t> skipSome(size_t atMost);

  /// @brief whether or not initializeCursor was called
  bool initializeCursorCalled() const;

  /// @brief add a block to the engine
  /// @returns added block
  ExecutionBlock* addBlock(std::unique_ptr<ExecutionBlock>);

  /// @brief set the register the final result of the query is stored in
  void resultRegister(RegisterId resultRegister);

  /// @brief get the register the final result of the query is stored in
  RegisterId resultRegister() const;

  /// @brief accessor to the memory recyler for AqlItemBlocks
  AqlItemBlockManager& itemBlockManager();

  ///  @brief collected execution stats
  void collectExecutionStats(ExecutionStats& other);
  /// should only be used by the RemoteExecutor and intenally
  ExecutionStats& globalStats() { return _execStats; }
  
  enum class ShutdownState : uint8_t {
    Legacy = 0, NotShutdown = 2, ShutdownSent = 4,
    Done = 8
  };
  
  void setShutdown(ShutdownState s) {
    _shutdownState.store(s, std::memory_order_relaxed);
  }
  ShutdownState shutdownState() const {
    return _shutdownState.load(std::memory_order_relaxed);
  }
  
  bool waitForSatellites(aql::QueryContext& query, Collection const* collection) const;
  
#ifdef USE_ENTERPRISE
  static void parallelizeTraversals(aql::Query& query, ExecutionPlan& plan,
                                    std::map<aql::ExecutionNodeId, aql::ExecutionNodeId>& aliases);
#endif
  
#ifdef ARANGODB_USE_GOOGLE_TESTS
  std::vector<ExecutionBlock*> const& blocksForTesting() const {
    return _blocks;
  }
#endif
  
 private:
  
  std::pair<ExecutionState, Result> shutdownDBServerQueries(int errorCode);

 private:
  /// @brief a pointer to the query
  QueryContext& _query;
  
  /// @brief memory recycler for AqlItemBlocks
  AqlItemBlockManager& _itemBlockManager;
  
  std::shared_ptr<SharedQueryState> _sharedState;

  /// @brief all blocks registered, used for memory management
  std::vector<ExecutionBlock*> _blocks;

  /// @brief root block of the engine
  ExecutionBlock* _root;

  /// @brief the register the final result of the query is stored in
  RegisterId _resultRegister;
  
  /// @brief server to snippet mapping
  MapRemoteToSnippet _dbServerMapping;

  /// @brief map of server to server-global query id
  std::map<std::string, QueryId> _serverToQueryId;
  
  ExecutionStats _execStats;
  
  Result _shutdownResult;
  
  /// @brief whether or not initializeCursor was called
  bool _initializeCursorCalled;
  
  std::atomic<ShutdownState> _shutdownState;
};
}  // namespace aql
}  // namespace arangodb

#endif
