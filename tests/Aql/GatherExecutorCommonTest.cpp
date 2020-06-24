////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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

#include "gtest/gtest.h"

#include "AqlExecutorTestCase.h"

#include "Aql/EmptyExecutorInfos.h"
#include "Aql/IdExecutor.h"
#include "Aql/ParallelUnsortedGatherExecutor.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/SortRegister.h"
#include "Aql/SortingGatherExecutor.h"
#include "Aql/UnsortedGatherExecutor.h"
#include "TestLambdaExecutor.h"

#include "Aql/SubqueryStartExecutor.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb::tests::aql {

enum class ExecutorType { UNSORTED, SORTING_HEAP, SORTING_MINELEMENT };

std::ostream& operator<<(std::ostream& out, ExecutorType type) {
  switch (type) {
    case ExecutorType::UNSORTED:
      return out << "UNSORTED";
    case ExecutorType::SORTING_HEAP:
      return out << "SORTING_HEAP";
    case ExecutorType::SORTING_MINELEMENT:
      return out << "SORTING_MINELEMENT";
    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
}

auto dependencyNumber = testing::Values(1, 2, 3);
auto parallelism = testing::Values(GatherNode::Parallelism::Serial,
                                   GatherNode::Parallelism::Parallel);

std::ostream& operator<<(std::ostream& out, GatherNode::Parallelism type) {
  switch (type) {
    case GatherNode::Parallelism::Serial:
      return out << "Serial";
    case GatherNode::Parallelism::Parallel:
      return out << "Parallel";
    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
}

auto combinations =
    ::testing::Combine(::testing::Values(ExecutorType::UNSORTED, ExecutorType::SORTING_HEAP,
                                         ExecutorType::SORTING_MINELEMENT),
                       dependencyNumber, parallelism);

using CommonParameter = std::tuple<ExecutorType, size_t, GatherNode::Parallelism>;

namespace {
/*
 * We produce the value in the following way:
 * we read the old value and multiply it by 10^6
 * We multiply the Gather Branch by 10^5
 * Then we add the number of execution in the current run.
 *
 * e.g:
 * Start: 1
 * SubqueryStart: 1.000.000 | 1.000.001
 * Gather Branch1: 1.000.000.000.000 | 1.000.000.000.001 | (SR 1.000.000)
 * | 1.000.001.000.000 | 1.000.001.000.001 | (SR 1.000.001)
 * Gather Branch2: 1.000.000.100.000 | 1.000.000.100.001 | (SR 1.000.000)
 * | 1.000.001.100.000 | 1.000.001.100.001 | (SR 1.000.001)
 *
 * This way we can produce up to 6 subqueries, each up to 10^5 rows
 * In a gather with upto 10 branches
 */
auto generateValue(std::vector<size_t> subqueryRuns, size_t branch) -> int64_t {
  int64_t val = std::accumulate(subqueryRuns.begin(), subqueryRuns.end(), 0ll,
                                [](int64_t old, size_t next) -> int64_t {
                                  TRI_ASSERT(old >= 0);
                                  return old * 1000000 + next;
                                });
  val += branch * 100000;
  TRI_ASSERT(val >= 0);
  return val;
}

#if 0
// This is the backwards function for generate value.
// I have not need this anywhere but maybe it is handy at one point
// so i decided to keep it in.
auto reverseValue(int64_t v) -> std::pair<std::vector<size_t>, size_t> {
  std::vector<size_t> subqueryValues{};
  size_t branch = (v / 100000) % 10;
  if (v == 0) {
    subqueryValues.emplace_back(0);
  }
  while (v > 0) {
    subqueryValues.emplace_back(v % 100000);
    v /= 1000000;
  }
  std::reverse(std::begin(subqueryValues), std::end(subqueryValues));

  return {subqueryValues, branch};
}
#endif

};  // namespace

class ResultMaps {
 public:
  ResultMaps() {}

  auto addValue(int64_t val) -> void {
    ASSERT_TRUE(val >= 0)
        << "Tried to insert a negative value, test stup broken";
    auto [it, inserted] = _data.back().emplace(val);
    ASSERT_TRUE(inserted) << "Tried to insert same value twice";
  }

  auto addShadowRow(int64_t val, uint64_t depth) -> void {
    _subqueryData.emplace_back(std::make_pair(val, depth));
    if (depth == 0) {
      _data.push_back({});
    }
  }

  auto testValueAllowed(int64_t val) -> void {
    ASSERT_LT(_dataReadIndex, _data.size());
    ASSERT_LT(_dataReadIndex, _dataProduced.size());
    auto& allowed = _data[_dataReadIndex];
    EXPECT_EQ(1, allowed.erase(val)) << "Did not find expected value " << val;
    _dataProduced[_dataReadIndex] = true;
  }

  auto testSubqueryValue(int64_t val, uint64_t depth) -> void {
    ASSERT_LT(_subqueryReadIndex, _subqueryData.size());
    auto const& [expVal, expDepth] = _subqueryData[_subqueryReadIndex];
    _subqueryReadIndex++;
    if (depth == 0) {
      // We consumed the ShadowRow for the data, let us check for the next set of data rows.
      _dataReadIndex++;
    }
    EXPECT_EQ(val, expVal);
    EXPECT_EQ(depth, expDepth);
  }

  auto testValuesSkippedInRun(size_t count, size_t index) -> void {
    ASSERT_LT(index, _data.size());
    EXPECT_EQ(_data[index].size(), count);
  }

  auto testSkippedInEachRun(size_t count) -> void {
    for (size_t i = 0; i < _data.size(); ++i) {
      testValuesSkippedInRun(count, i);
    }
  }

  auto testAllValuesProduced() -> void { testSkippedInEachRun(0); }

  auto testAllValuesProducedOfRun(size_t index) -> void {
    testValuesSkippedInRun(0, index);
  }

  auto testAllValuesSkippedInRun(size_t index) -> void {
    ASSERT_LT(index, _dataProduced.size());
    EXPECT_FALSE(_dataProduced[index]);
  }

  auto testAllValuesSkipped() -> void {
    for (size_t i = 0; i < _data.size(); ++i) {
      testAllValuesSkippedInRun(i);
    }
  }

  auto skipOverSubquery(size_t depth, size_t times = 1) -> void {
    while (times > 0 && _subqueryReadIndex < _subqueryData.size()) {
      auto const& [value, d] = _subqueryData[_subqueryReadIndex];
      if (d > depth) {
        // Cannot skip over outer shadowrow.
        break;
      }
      if (d == 0) {
        // Skipped over data
        _dataReadIndex++;
      }
      if (d == depth) {
        times--;
      }
      _subqueryReadIndex++;
    }
  }

  auto popLastInNestedCase() -> void {
    if (!_subqueryData.empty()) {
      TRI_ASSERT(_data.back().empty());
      _data.pop_back();
    }
    _dataProduced.resize(_data.size(), false);
  }

  auto logContents() -> void {
    LOG_DEVEL << "Expected Data:";
    size_t subqueryIndex = 0;
    for (auto const& data : _data) {
      logData(data);
      subqueryIndex = logConsecutiveShadowRows(subqueryIndex);
    }
  }

 private:
  auto logData(std::unordered_set<int64_t> const& data) -> void {
    if (!data.empty()) {
      LOG_DEVEL << std::accumulate(std::next(data.begin()), data.end(),
                                   std::to_string(*data.begin()),
                                   [](std::string prefix, int64_t value) -> std::string {
                                     return std::move(prefix) + ", " + std::to_string(value);
                                   });
    } else {
      LOG_DEVEL << "No Data";
    }
  }

  auto logConsecutiveShadowRows(size_t startIndex) -> size_t {
    if (_subqueryData.empty()) {
      // No shadowRows, we can only have one call here
      TRI_ASSERT(startIndex == 0);
      return 1;
    }
    // If we get here we are requried to ahve at least one shadowRow
    TRI_ASSERT(startIndex < _subqueryData.size());
    {
      auto const& [value, depth] = _subqueryData[startIndex];
      LOG_DEVEL << "ShadowRow: Depth: " << depth << "Value: " << value;
    }
    ++startIndex;
    while (startIndex < _subqueryData.size()) {
      auto const& [value, depth] = _subqueryData[startIndex];
      if (depth == 0) {
        // Print this on next round
        return startIndex;
      }
      LOG_DEVEL << "ShadowRow: Depth: " << depth << "Value: " << value;
      startIndex++;
    }

    return startIndex;
  }

 private:
  std::vector<std::unordered_set<int64_t>> _data{{}};
  std::vector<std::pair<int64_t, uint64_t>> _subqueryData{};
  std::vector<bool> _dataProduced{};
  size_t _dataReadIndex{0};
  size_t _subqueryReadIndex{0};
};

class CommonGatherExecutorTest
    : public AqlExecutorTestCaseWithParam<CommonParameter, false> {
 protected:
  CommonGatherExecutorTest()
      : _useLogging(false) /* activates result logging */ {}

  auto getExecutor(std::deque<size_t> subqueryRuns, size_t dataSize = 10)
      -> std::pair<std::unique_ptr<ExecutionBlock>, ResultMaps> {
    auto exec = buildExecutor(subqueryRuns.size() + 1);
    TRI_ASSERT(exec != nullptr);
    auto res = generateData(*exec, subqueryRuns, dataSize);
    return {std::move(exec), res};
  }

  auto assertResultValid(SharedAqlItemBlockPtr block, ResultMaps& result) -> void {
    if (block != nullptr) {
      for (size_t row = 0; row < block->size(); ++row) {
        if (block->isShadowRow(row)) {
          ShadowAqlItemRow in{block, row};
          auto val = in.getValue(0);
          ASSERT_TRUE(val.isNumber());
          int64_t v = val.toInt64();
          auto depthVal = in.getDepth();
          result.testSubqueryValue(v, depthVal);
        } else {
          InputAqlItemRow in{block, row};
          auto val = in.getValue(0);
          ASSERT_TRUE(val.isNumber());
          int64_t v = val.toInt64();
          result.testValueAllowed(v);
        }
      }
    }
  }

  auto toCallList(AqlCall const& call) -> AqlCallList {
    return AqlCallList{call};
  }

  auto fetchAllCall() -> AqlCallList { return toCallList(AqlCall{}); }

  auto skipThenFetchCall(size_t offset) -> AqlCallList {
    return toCallList(AqlCall{offset});
  }

  auto executeUntilResponse(ExecutionBlock* exec, AqlCallStack stack)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
    ExecutionState state = ExecutionState::WAITING;
    SkipResult skipped{};
    SharedAqlItemBlockPtr block{nullptr};
    while (state == ExecutionState::WAITING) {
      std::tie(state, skipped, block) = exec->execute(stack);
      TRI_ASSERT(state != ExecutionState::WAITING || skipped.nothingSkipped());
      TRI_ASSERT(state != ExecutionState::WAITING || block == nullptr);
    }
    return {state, skipped, block};
  }

 private:
  auto getType() -> ExecutorType {
    auto const& [type, clients, parallelism] = GetParam();
    return type;
  }

  auto getClients() -> size_t {
    auto const& [type, clients, parallelism] = GetParam();
    return clients;
  }

  auto parallelism() -> GatherNode::Parallelism {
    auto const& [type, clients, parallelism] = GetParam();
    return parallelism;
  }

  /**
   * @brief Generate the data values.
   *        Every entry in the vector is a seperate subquery run
   *        in the set every possible value is stored exactly once.
   *
   * @param block The block where we inject dependencies
   * @param subqueryRuns Number of shadowRows on every level (0 => mainquery)
   *                     For every outer run we will have all of the inner runs
   *                     e.g. {2, 5} will have 2 main query runs, each with 5 subquery runs.
   *                     Empty vector means no shadowRows
   * @return std::pair<std::vector<std::unordered_map<size_t>>, std::vector<size_t>>
   */
  auto generateData(ExecutionBlock& block, std::deque<size_t> subqueryRuns,
                    size_t dataSize) -> ResultMaps {
    // Parent will be the shared ancestor of all following blocks
    // and will move through the list of blocks
    ExecutionBlock* parent = nullptr;
    size_t nestingLevel = 1;
    {
      // We start with Value 0
      SharedAqlItemBlockPtr inBlock = buildBlock<1>(itemBlockManager, {{0}});
      std::deque<SharedAqlItemBlockPtr> blockDeque;
      blockDeque.push_back(inBlock);
      auto producer = std::make_unique<WaitingExecutionBlockMock>(
          fakedQuery->rootEngine(), generateNodeDummy(), std::move(blockDeque),
          WaitingExecutionBlockMock::WaitingBehaviour::NEVER);
      parent = producer.get();
      _blockLake.emplace_back(std::move(producer));
    }

    // Now we add a producer for each subqueryLevel.
    for (auto const& number : subqueryRuns) {
      // Add Producer
      auto prod = generateProducer(number, 0, nestingLevel);
      prod->addDependency(parent);

      // Add SubqueryStart
      nestingLevel++;
      auto subq = generateSubqueryStart(nestingLevel);
      subq->addDependency(prod.get());

      parent = subq.get();
      _blockLake.emplace_back(std::move(subq));
      _blockLake.emplace_back(std::move(prod));
    }

    {
      // Now add the Scatter
      auto scatter = generateScatter(nestingLevel);
      scatter->addDependency(parent);
      parent = scatter.get();
      _blockLake.emplace_back(std::move(scatter));
    }

    for (size_t i = 0; i < getClients(); ++i) {
      // Now add the branches
      auto consumer = generateConsumer(i, nestingLevel);
      consumer->addDependency(parent);

      auto prod = generateProducer(dataSize, i, nestingLevel);
      prod->addDependency(consumer.get());

      block.addDependency(prod.get());

      _blockLake.emplace_back(std::move(consumer));
      _blockLake.emplace_back(std::move(prod));
    }

    // ResultMaps
    ResultMaps res;
    generateExpectedData(res, subqueryRuns, dataSize, {});
    res.popLastInNestedCase();
    if (_useLogging) {
      res.logContents();
    }
    return res;
  }

  auto generateExpectedData(ResultMaps& results,
                            std::deque<size_t> subqueryRuns, size_t dataSize,
                            std::vector<size_t> currentSubqueryValues) -> void {
    if (subqueryRuns.empty()) {
      currentSubqueryValues.emplace_back(0);
      for (size_t i = 0; i < dataSize; ++i) {
        // We modify the topmost element
        currentSubqueryValues.back() = i;
        for (size_t branch = 0; branch < getClients(); ++branch) {
          results.addValue(::generateValue(currentSubqueryValues, branch));
        }
      }
    } else {
      size_t runs = subqueryRuns.front();
      subqueryRuns.pop_front();
      currentSubqueryValues.emplace_back(0);
      for (size_t i = 0; i < runs; ++i) {
        currentSubqueryValues.back() = i;
        // Fill in data from inner subqueries
        generateExpectedData(results, subqueryRuns, dataSize, currentSubqueryValues);
        // Fill in ShadowRow
        results.addShadowRow(::generateValue(currentSubqueryValues, 0),
                             subqueryRuns.size());
      }
    }
  }

  auto buildRegisterInfos(size_t nestingLevel) -> RegisterInfos {
    TRI_ASSERT(nestingLevel > 0);
    RegIdSetStack toKeepStack{};
    for (size_t i = 0; i < nestingLevel; ++i) {
      toKeepStack.emplace_back(RegIdSet{0});
    }
    return RegisterInfos(RegIdSet{0}, {}, 1, 1, {}, std::move(toKeepStack));
  }

  auto buildProducerRegisterInfos(size_t nestingLevel) -> RegisterInfos {
    TRI_ASSERT(nestingLevel > 0);
    RegIdSetStack toKeepStack{};
    for (size_t i = 1; i < nestingLevel; ++i) {
      toKeepStack.emplace_back(RegIdSet{0});
    }
    toKeepStack.emplace_back(RegIdSet{});
    return RegisterInfos(RegIdSet{0}, RegIdSet{0}, 1, 1, {}, std::move(toKeepStack));
  }

  auto buildExecutor(size_t nestingLevel) -> std::unique_ptr<ExecutionBlock> {
    auto regInfos = buildRegisterInfos(nestingLevel);
    switch (getType()) {
      case ExecutorType::UNSORTED:
        return unsortedExecutor(std::move(regInfos));
      case ExecutorType::SORTING_HEAP:
        return sortedExecutor(std::move(regInfos), GatherNode::SortMode::Heap);
      case ExecutorType::SORTING_MINELEMENT:
        return sortedExecutor(std::move(regInfos), GatherNode::SortMode::MinElement);
      default:
        THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }
  }

  auto unsortedExecutor(RegisterInfos&& regInfos) -> std::unique_ptr<ExecutionBlock> {
    if (parallelism() == GatherNode::Parallelism::Parallel) {
      return std::make_unique<ExecutionBlockImpl<ParallelUnsortedGatherExecutor>>(
          fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::GATHER),
          std::move(regInfos), EmptyExecutorInfos());
    }
    IdExecutorInfos execInfos{false};
    return std::make_unique<ExecutionBlockImpl<UnsortedGatherExecutor>>(
        fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::GATHER),
        std::move(regInfos), std::move(execInfos));
  }

  auto sortedExecutor(RegisterInfos&& regInfos, GatherNode::SortMode sortMode)
      -> std::unique_ptr<ExecutionBlock> {
    std::vector<SortRegister> sortRegister;
    sortRegister.emplace_back(SortRegister{0, SortElement{nullptr, true}});

    auto executorInfos =
        SortingGatherExecutorInfos(std::move(sortRegister), *fakedQuery.get(),
                                   sortMode, 0, parallelism());
    return std::make_unique<ExecutionBlockImpl<SortingGatherExecutor>>(
        fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::GATHER),
        std::move(regInfos), std::move(executorInfos));
  }

  auto generateProducer(int64_t numDataRows, size_t branch, size_t nestingLevel)
      -> std::unique_ptr<ExecutionBlock> {
    TRI_ASSERT(numDataRows > 0);

    // The below code only works if we do not have multithreading within
    // the same branch.
    // The access to val is unprotected.
    auto val = std::make_shared<int64_t>(0);
    ProduceCall produce =
        [branch, numDataRows,
         val](AqlItemBlockInputRange& inputRange,
              OutputAqlItemRow& output) -> std::tuple<ExecutorState, NoStats, AqlCall> {
      ExecutorState state = ExecutorState::HASMORE;
      InputAqlItemRow input{CreateInvalidInputRowHint{}};

      while (inputRange.hasDataRow() && *val < numDataRows && !output.isFull()) {
        // This executor is passthrough. it has enough place to write.
        TRI_ASSERT(!output.isFull());
        std::tie(state, input) = inputRange.peekDataRow();
        TRI_ASSERT(input.isInitialized());
        auto oldVal = input.getValue(0);
        TRI_ASSERT(oldVal.isNumber());
        int64_t old = oldVal.toInt64();
        TRI_ASSERT(old >= 0);
        old *= 1000000;
        old += branch * 100000;
        old += (*val)++;
        AqlValue v{AqlValueHintInt(old)};
        AqlValueGuard guard(v, true);
        output.moveValueInto(0, input, guard);
        output.advanceRow();

        if (*val == numDataRows) {
          std::ignore = inputRange.nextDataRow(AqlItemBlockInputRange::HasDataRow{});
        }
      }

      return {inputRange.upstreamState(), NoStats{}, output.getClientCall()};
    };

    SkipCall skip = [numDataRows, val](AqlItemBlockInputRange& inputRange, AqlCall& call)
        -> std::tuple<ExecutorState, NoStats, size_t, AqlCall> {
      ExecutorState state = ExecutorState::HASMORE;
      InputAqlItemRow input{CreateInvalidInputRowHint{}};

      while (inputRange.hasDataRow() && *val < numDataRows && call.needSkipMore()) {
        // This executor is passthrough. it has enough place to write.
        std::tie(state, input) = inputRange.peekDataRow();
        TRI_ASSERT(input.isInitialized());
        auto oldVal = input.getValue(0);
        TRI_ASSERT(oldVal.isNumber());
        (*val)++;
        call.didSkip(1);

        if (*val == numDataRows) {
          std::ignore = inputRange.nextDataRow(AqlItemBlockInputRange::HasDataRow{});
        }
      }
      // We need all data from upstream and cannot forward skip
      return {inputRange.upstreamState(), NoStats{}, call.getSkipCount(), AqlCall{}};
    };

    ResetCall reset = [val]() -> void { *val = 0; };
    LambdaSkipExecutorInfos executorInfos{produce, skip, reset};
    return std::make_unique<ExecutionBlockImpl<TestLambdaSkipExecutor>>(
        fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::ENUMERATE_COLLECTION),
        buildProducerRegisterInfos(nestingLevel), std::move(executorInfos));
  }

  auto generateSubqueryStart(size_t nestingLevel) -> std::unique_ptr<ExecutionBlock> {
    return std::make_unique<ExecutionBlockImpl<SubqueryStartExecutor>>(
        fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::SUBQUERY_START),
        buildRegisterInfos(nestingLevel), buildRegisterInfos(nestingLevel));
  }

  auto generateScatter(size_t nestingLevel) -> std::unique_ptr<ExecutionBlock> {
    std::vector<std::string> clientIds{};
    for (size_t i = 0; i < getClients(); ++i) {
      clientIds.emplace_back(std::to_string(i));
    }
    ScatterExecutorInfos execInfos(std::move(clientIds));

    return std::make_unique<ExecutionBlockImpl<ScatterExecutor>>(
        fakedQuery->rootEngine(), generateScatterNodeDummy(),
        buildRegisterInfos(nestingLevel), std::move(execInfos));
  }

  auto generateConsumer(size_t branch, size_t nestingLevel)
      -> std::unique_ptr<ExecutionBlock> {
    IdExecutorInfos execInfos(false, 0, std::to_string(branch), branch == 0);
    return std::make_unique<ExecutionBlockImpl<IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>>(
        fakedQuery->rootEngine(), generateNodeDummy(ExecutionNode::DISTRIBUTE_CONSUMER),
        buildRegisterInfos(nestingLevel), std::move(execInfos));
  }

  // Memory Management for ExecutionBlocks
  std::vector<std::unique_ptr<ExecutionBlock>> _blockLake;
  // Activate result logging
  bool _useLogging{false};

};  // namespace arangodb::tests::aql

INSTANTIATE_TEST_CASE_P(CommonGatherTests, CommonGatherExecutorTest, combinations);

TEST_P(CommonGatherExecutorTest, get_all) {
  auto [exec, result] = getExecutor({});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{fetchAllCall()};
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);

    ASSERT_TRUE(skipped.nothingSkipped());
    assertResultValid(block, result);
  }
  result.testAllValuesProduced();
}

TEST_P(CommonGatherExecutorTest, get_all_sub_1) {
  auto [exec, result] = getExecutor({4});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{fetchAllCall()};
  stack.pushCall(fetchAllCall());
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);

    ASSERT_TRUE(skipped.nothingSkipped());
    assertResultValid(block, result);
  }
  result.testAllValuesProduced();
}

TEST_P(CommonGatherExecutorTest, get_all_sub_2) {
  auto [exec, result] = getExecutor({3, 5});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{fetchAllCall()};
  stack.pushCall(fetchAllCall());
  stack.pushCall(fetchAllCall());
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);

    ASSERT_TRUE(skipped.nothingSkipped());
    assertResultValid(block, result);
  }
  result.testAllValuesProduced();
}

TEST_P(CommonGatherExecutorTest, skip_data) {
  auto [exec, result] = getExecutor({});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{skipThenFetchCall(5)};
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);

    ASSERT_FALSE(skipped.nothingSkipped());
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(0), 5);
    assertResultValid(block, result);
  }
  result.testSkippedInEachRun(5);
}

TEST_P(CommonGatherExecutorTest, skip_data_sub_1) {
  auto [exec, result] = getExecutor({4});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{fetchAllCall()};
  stack.pushCall(skipThenFetchCall(5));
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);

    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(0), 5);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(1), 0);
    assertResultValid(block, result);
  }
  result.testSkippedInEachRun(5);
}

TEST_P(CommonGatherExecutorTest, skip_data_sub_2) {
  auto [exec, result] = getExecutor({3, 5});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{fetchAllCall()};
  stack.pushCall(fetchAllCall());
  stack.pushCall(skipThenFetchCall(5));
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped{};
  SharedAqlItemBlockPtr block{nullptr};
  while (state != ExecutionState::DONE) {
    // In this test we do not care for waiting.
    std::tie(state, skipped, block) = exec->execute(stack);
    assertResultValid(block, result);
    EXPECT_EQ(skipped.getSkipCount(), 5);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(0), 5);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(1), 0);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(2), 0);
  }
  result.testSkippedInEachRun(5);
}

TEST_P(CommonGatherExecutorTest, skip_main_query_sub_1) {
  auto [exec, result] = getExecutor({3});

  // Default Stack, fetch all unlimited
  AqlCallStack stack{skipThenFetchCall(1)};
  stack.pushCall(fetchAllCall());
  result.skipOverSubquery(0, 1);
  {
    auto [state, skipped, block] = executeUntilResponse(exec.get(), stack);
    // In the first round we need to skip
    EXPECT_EQ(skipped.getSkipCount(), 0);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(1), 1);
    assertResultValid(block, result);
    // we skipped 1 count it.
    stack.modifyCallAtDepth(1).offset -= 1;
    EXPECT_EQ(state, ExecutionState::HASMORE);
  }
  {
    auto [state, skipped, block] = executeUntilResponse(exec.get(), stack);
    // In the second round we do not need to skip any more
    EXPECT_EQ(skipped.getSkipCount(), 0);
    EXPECT_EQ(skipped.getSkipOnSubqueryLevel(1), 0);
    assertResultValid(block, result);
    EXPECT_EQ(state, ExecutionState::DONE);
  }

  // We can do this in one go, there is no need to recall again.
  result.testAllValuesSkippedInRun(0);
  result.testValuesSkippedInRun(0, 1);
  result.testValuesSkippedInRun(0, 2);
}

}  // namespace arangodb::tests::aql
