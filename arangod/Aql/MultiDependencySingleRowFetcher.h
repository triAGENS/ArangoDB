////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_MULTI_DEPENDENCY_SINGLE_ROW_FETCHER_H
#define ARANGOD_AQL_MULTI_DEPENDENCY_SINGLE_ROW_FETCHER_H

#include "Aql/AqlCallSet.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/MultiAqlItemBlockInputRange.h"
#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"

#include <memory>

namespace arangodb::aql {

class AqlItemBlock;
template <BlockPassthrough>
class DependencyProxy;
class ShadowAqlItemRow;
class SkipResult;

/**
 * @brief Interface for all AqlExecutors that do need one
 *        row at a time from every dependency in order to make progress.
 */
class MultiDependencySingleRowFetcher {
 private:
  class UpstreamSkipReport {
   public:
    UpstreamSkipReport();
    ~UpstreamSkipReport() = default;

    auto isInitialized() const -> bool;

    auto initialize(size_t depth) -> void;

    auto getSkipped(size_t subqueryDepth) const -> size_t;

    auto getFullCount(size_t subqueryDepth) const-> size_t;

    auto clearCounts(size_t subqueryDepth) -> void;

    auto setSkipped(size_t subqueryDepth, size_t skipped) -> void;
    auto setFullCount(size_t subqueryDepth, size_t skipped) -> void;

   private:
    bool _isInitialized{false};
    std::vector<std::pair<size_t, size_t>> _report;
  };

  /**
   * @brief helper struct to contain all information about dependency-states
   */
  struct DependencyInfo {
   public:
    DependencyInfo();
    ~DependencyInfo() = default;
    /**
     * @brief Holds state returned by the last execute() call.
     *        This is similar to ExecutionBlock::_upstreamState, but can also be
     *        WAITING.
     *        Part of the Fetcher, and may be moved if the Fetcher
     * implementations are moved into separate classes.
     */
    ExecutionState _upstreamState;

    /**
     * @brief Input block currently in use. Used for memory management by the
     *        SingleRowFetcher. May be moved if the Fetcher implementations
     *        are moved into separate classes.
     */
    SharedAqlItemBlockPtr _currentBlock;

    /**
     * @brief Index of the row to be returned next. This is valid
     *        iff _currentBlock != nullptr and it's smaller or equal than
     *        _currentBlock->size(). May be moved if the Fetcher implementations
     *        are moved into separate classes.
     */
    size_t _rowIndex;
  };

 public:
  using DataRange = MultiAqlItemBlockInputRange;
  explicit MultiDependencySingleRowFetcher(DependencyProxy<BlockPassthrough::Disable>& executionBlock);
  TEST_VIRTUAL ~MultiDependencySingleRowFetcher() = default;

  void init();

 protected:
  // only for testing! Does not initialize _dependencyProxy!
  MultiDependencySingleRowFetcher();

 public:
  // May only be called once, after the dependencies are injected.
  void initDependencies();

  size_t numberDependencies();

  [[nodiscard]] auto execute(AqlCallStack const&, AqlCallSet const&)
      -> std::tuple<ExecutionState, SkipResult, std::vector<std::pair<size_t, AqlItemBlockInputRange>>>;

  [[nodiscard]] auto upstreamState() const -> ExecutionState;

  auto resetDidReturnSubquerySkips(size_t shadowRowDepth) -> void;

 private:
  DependencyProxy<BlockPassthrough::Disable>* _dependencyProxy;

  /**
   * @brief Holds the information for all dependencies
   */
  std::vector<DependencyInfo> _dependencyInfos;
  std::vector<ExecutionState> _dependencyStates;
  std::vector<UpstreamSkipReport> _dependencySkipReports;

  UpstreamSkipReport _maximumSkipReport;

  /// @brief Only needed for parallel executors; could be omitted otherwise
  ///        It's size is >0 after init() is called, and this is currently used
  ///        in initOnce() to make sure that init() is called exactly once.
  std::vector<std::optional<std::pair<AqlCallStack, AqlCallList>>> _callsInFlight;

  bool _didReturnSubquerySkips{false};

 private:
  [[nodiscard]] auto executeForDependency(size_t dependency, AqlCallStack const& stack, AqlCallList clientCall)
      -> std::tuple<ExecutionState, SkipResult, AqlItemBlockInputRange>;

  /**
   * @brief Delegates to ExecutionBlock::getNrInputRegisters()
   */
  RegisterCount getNrInputRegisters() const;

  bool indexIsValid(DependencyInfo const& info) const;

  bool isDone(DependencyInfo const& info) const;


  AqlCallStack adjustStackWithSkipReport(AqlCallStack const& stack, const size_t dependency);

  void reportSkipForDependency(AqlCallStack const& originalStack,
                               SkipResult const& skipped, const size_t dependency);
};

}  // namespace arangodb::aql

#endif  // ARANGOD_AQL_SINGLE_ROW_FETCHER_H
