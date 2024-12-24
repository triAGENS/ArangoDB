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

#include "Aql/AsyncPrefetchSlotsManager.h"
#include "Aql/ExecutionState.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/types.h"
#include "Basics/Result.h"
#include "Cluster/CallbackGuard.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace arangodb {
template<typename>
struct async;
}

namespace arangodb::aql {

class AqlCallStack;
class AqlItemBlock;
class AqlItemBlockManager;
class BindParameters;
class ExecutionBlock;
class ExecutionNode;
class ExecutionPlan;
struct ExecutionStats;
class GraphNode;
class Query;
class QueryContext;
class QueryRegistry;
class SkipResult;
class SharedQueryState;

class ExecutionEngine {
 public:
  ExecutionEngine(ExecutionEngine const&) = delete;
  ExecutionEngine& operator=(ExecutionEngine const&) = delete;

  /// @brief create the engine
  ExecutionEngine(EngineId eId, QueryContext& query,
                  AqlItemBlockManager& itemBlockManager,
                  std::shared_ptr<SharedQueryState> sharedState);

  /// @brief destroy the engine, frees all assigned blocks
  TEST_VIRTUAL ~ExecutionEngine();

  void leaseAsyncPrefetchSlots(size_t value);

  size_t asyncPrefetchSlotsLeased() const noexcept;

  // @brief create an execution engine from a plan
  static async<void> instantiateFromPlan(Query& query, ExecutionPlan& plan,
                                         bool planRegisters);

  /// @brief Prepares execution blocks for executing provided plan
  /// @param plan plan to execute, should be without cluster nodes. Only local
  /// execution without db-objects access!
  void initFromPlanForCalculation(ExecutionPlan& plan);

  TEST_VIRTUAL Result createBlocks(std::vector<ExecutionNode*> const& nodes,
                                   MapRemoteToSnippet const& queryIds);

  EngineId engineId() const { return _engineId; }

  /// @brief get the root block
  TEST_VIRTUAL ExecutionBlock* root() const;

  /// @brief set the root block
  TEST_VIRTUAL void root(ExecutionBlock* root);

  /// @brief get the query
  QueryContext& getQuery() const;

  std::shared_ptr<SharedQueryState> const& sharedState() const;

  /// @brief initializeCursor, could be called multiple times
  std::pair<ExecutionState, Result> initializeCursor(
      SharedAqlItemBlockPtr&& items, size_t pos);

  auto execute(AqlCallStack const& stack)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  auto executeForClient(AqlCallStack const& stack, std::string const& clientId)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  /// @brief whether or not initializeCursor was called
  bool initializeCursorCalled() const;

  /// @brief add a block to the engine
  /// @returns added block
  ExecutionBlock* addBlock(std::unique_ptr<ExecutionBlock>);

  /// @brief clears all blocks and root in Engine
  void reset();

  /// @brief set the register the final result of the query is stored in
  void resultRegister(RegisterId resultRegister);

  /// @brief get the register the final result of the query is stored in
  RegisterId resultRegister() const;

  /// @brief accessor to the memory recyler for AqlItemBlocks
  AqlItemBlockManager& itemBlockManager();

  ///  @brief collected execution stats
  void collectExecutionStats(ExecutionStats& other);

  std::vector<arangodb::cluster::CallbackGuard>& rebootTrackers();

#ifdef USE_ENTERPRISE
  static bool parallelizeGraphNode(
      aql::Query& query, ExecutionPlan& plan, aql::GraphNode* graphNode,
      std::map<aql::ExecutionNodeId, aql::ExecutionNodeId>& aliases);

  static void parallelizeTraversals(
      aql::Query& query, ExecutionPlan& plan,
      std::map<aql::ExecutionNodeId, aql::ExecutionNodeId>& aliases);
#endif

#ifdef ARANGODB_USE_GOOGLE_TESTS
  std::vector<std::unique_ptr<ExecutionBlock>> const& blocksForTesting() const {
    return _blocks;
  }
#endif

 private:
  /// @brief  optimizes root node: in case of single RETURN statement
  /// makes it return value directly and not copy it to output register.
  /// Also sets root execution block as engine root
  void setupEngineRoot(ExecutionBlock& planRoot);

  static void initializeConstValueBlock(ExecutionPlan& plan,
                                        BindParameters const& bindParameters,
                                        AqlItemBlockManager& mgr);

  EngineId const _engineId;

  /// @brief a pointer to the query
  QueryContext& _query;

  /// @brief memory recycler for AqlItemBlocks
  AqlItemBlockManager& _itemBlockManager;

  std::shared_ptr<SharedQueryState> _sharedState;

  /// @brief all blocks registered, used for memory management
  std::vector<std::unique_ptr<ExecutionBlock>> _blocks;

  /// @brief root block of the engine
  ExecutionBlock* _root;

  /// @brief reboot trackers for DB servers participating in the query
  std::vector<arangodb::cluster::CallbackGuard> _rebootTrackers;

  /// @brief the register the final result of the query is stored in
  RegisterId _resultRegister;

  /// @brief whether or not initializeCursor was called
  bool _initializeCursorCalled;

  AsyncPrefetchSlotsManager& _asyncPrefetchSlotsManager;

  AsyncPrefetchSlotsReservation _asyncPrefetchSlotsReservation;
};

}  // namespace arangodb::aql
