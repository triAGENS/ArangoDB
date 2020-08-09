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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_EXECUTION_BLOCK_H
#define ARANGOD_AQL_EXECUTION_BLOCK_H 1

#include "Aql/ExecutionState.h"
#include "Aql/ExecutionNodeStats.h"
#include "Aql/SkipResult.h"
#include "Basics/Result.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace arangodb {
namespace transaction {
class Methods;
}

namespace aql {
class AqlCallList;
class AqlCallStack;
class InputAqlItemRow;
class ExecutionEngine;
class ExecutionNode;
struct ExecutionStats;
class SharedAqlItemBlockPtr;

class ExecutionBlock {
 public:
  ExecutionBlock(ExecutionEngine*, ExecutionNode const*);

  virtual ~ExecutionBlock();

  ExecutionBlock(ExecutionBlock const&) = delete;
  ExecutionBlock& operator=(ExecutionBlock const&) = delete;

 public:
  /// @brief batch size value
  static constexpr size_t ProductionDefaultBatchSize = 1000;

#ifdef ARANGODB_USE_GOOGLE_TESTS
  // when we compile the tests, we want to make the batch size adjustable
  static size_t DefaultBatchSize;
  static void setDefaultBatchSize(size_t value) { DefaultBatchSize = value; }
#else
  // batch size is hard-coded for release builds
  static constexpr size_t DefaultBatchSize = ProductionDefaultBatchSize;
#endif

  /// @brief Number to use when we skip all. Should really be inf, but don't
  /// use something near std::numeric_limits<size_t>::max() to avoid overflows
  /// in calculations.
  /// This is used as an argument for skipRowsRange(), e.g. when counting everything.
  /// Setting this to any other value >0 does not (and must not) affect the
  /// results. It's only to reduce the number of necessary skipRowsRange calls.
  [[nodiscard]] static constexpr inline size_t SkipAllSize() {
    return 1000000000;
  }

  /// @brief Methods for execution
  /// Lifecycle is:
  ///    CONSTRUCTOR
  ///    then the ExecutionEngine automatically calls
  ///      initialize() once, including subqueries
  ///    possibly repeat many times:
  ///      initializeCursor(...)   (optionally with bind parameters)
  ///      // use cursor functionality
  ///    then the ExecutionEngine automatically calls
  ///      shutdown()
  ///    DESTRUCTOR

  /// @brief initializeCursor, could be called multiple times
  [[nodiscard]] virtual std::pair<ExecutionState, Result> initializeCursor(InputAqlItemRow const& input);

  /// @brief shutdown, will be called exactly once for the whole query
  [[nodiscard]] virtual std::pair<ExecutionState, Result> shutdown(int errorCode);

  [[nodiscard]] ExecutionState getHasMoreState();

  // TODO: Can we get rid of this? Problem: Subquery Executor is using it.
  [[nodiscard]] ExecutionNode const* getPlanNode() const;

  /// @brief add a dependency
  void addDependency(ExecutionBlock* ep);

  /// @brief main function to produce data in this ExecutionBlock.
  ///        It gets the AqlCallStack defining the operations required in every
  ///        subquery level. It will then perform the requested amount of offset, data and fullcount.
  ///        The AqlCallStack is copied on purpose, so this block can modify it.
  ///        Will return
  ///        1. state:
  ///          * WAITING: We have async operation going on, nothing happend, please call again
  ///          * HASMORE: Here is some data in the request range, there is still more, if required call again
  ///          * DONE: Here is some data, and there will be no further data available.
  ///        2. SkipResult: Amount of documents skipped.
  ///        3. SharedAqlItemBlockPtr: The next data block.
  virtual std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(AqlCallStack const& stack, AqlCallList clientCall) = 0;
  
  virtual void collectExecStats(ExecutionStats&) const;
  [[nodiscard]] bool isInSplicedSubquery() const noexcept;
  
  [[nodiscard]] auto printBlockInfo() const -> std::string const;
  [[nodiscard]] auto printTypeInfo() const -> std::string const;
  
 protected:
  
  // Trace the start of a execute call
  void traceExecuteBegin(AqlCallStack const& stack,
                         AqlCallList const& clientCall,
                         std::string const& clientId = "");

  // Trace the end of a execute call, potentially with result
  void traceExecuteEnd(std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> const& result,
                       std::string const& clientId = "");

 protected:
  /// @brief the execution engine
  ExecutionEngine* _engine;

  /// @brief the Result returned during the shutdown phase. Is kept for multiple
  ///        waiting phases.
  Result _shutdownResult;

  /// @brief the execution state of the dependency
  ///        used to determine HASMORE or DONE better
  ExecutionState _upstreamState;
  
  /// @brief profiling level
  uint16_t _profile;

  /// @brief if this is set, we are done, this is reset to false by execute()
  bool _done;

  bool _isInSplicedSubquery;

  /// @brief our corresponding ExecutionNode node
  ExecutionNode const* _exeNode;  // TODO: Can we get rid of this? Problem: Subquery Executor is using it.

  /// @brief our dependent nodes
  std::vector<ExecutionBlock*> _dependencies;

  /// @brief position in the dependencies while iterating through them
  ///        used in initializeCursor and shutdown.
  ///        Needs to be set to .end() everytime we modify _dependencies
  std::vector<ExecutionBlock*>::iterator _dependencyPos;
  
  ExecutionNodeStats _execNodeStats;
};

}  // namespace aql
}  // namespace arangodb

#endif
