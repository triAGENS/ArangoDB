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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "AqlExecutorTestCase.h"
#include "AqlItemBlockHelper.h"
#include "RowFetcherHelper.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/IdExecutor.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/RegisterInfos.h"
#include "Aql/ResourceUsage.h"
#include "Aql/Stats.h"

#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb::tests::aql {

using TestParam = std::tuple<size_t,         // The input data 0 -> number
                             ExecutorState,  // The upstream state
                             AqlCall,        // The client Call,
                             bool  // flag to decide if we need to do couting
                             >;

class IdExecutorTestCombiner : public AqlExecutorTestCaseWithParam<TestParam> {
 protected:
  auto prepareInputRange() -> AqlItemBlockInputRange {
    auto input = getInput();
    if (input == 0) {
      // no input
      return AqlItemBlockInputRange{getUpstreamState()};
    }
    MatrixBuilder<1> matrix;
    for (int i = 0; i < static_cast<int>(input); ++i) {
      matrix.emplace_back(RowBuilder<1>{i});
    }
    SharedAqlItemBlockPtr block = buildBlock<1>(manager(), std::move(matrix));
    TRI_ASSERT(getCall().getSkipCount() == 0);
    return AqlItemBlockInputRange{getUpstreamState(), 0, block, 0};
  }

  auto doCount() -> bool {
    auto const& [a, b, c, doCount] = GetParam();
    return doCount;
  }

  auto makeRegisterInfos() -> RegisterInfos {
    return RegisterInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
  }

  auto makeExecutorInfos() -> IdExecutorInfos {
    return IdExecutorInfos{doCount()};
  }

  auto getInput() -> size_t {
    auto const& [input, a, b, c] = GetParam();
    return input;
  }

  auto getCall() -> AqlCall {
    auto const& [a, b, call, c] = GetParam();
    return call;
  }

  auto getUpstreamState() -> ExecutorState {
    auto const& [a, state, b, c] = GetParam();
    return state;
  }

  auto getExpectedState() -> ExecutionState {
    auto call = getCall();
    auto available = getInput();
    if (call.needsFullCount() || call.getOffset() + call.getLimit() >= available) {
      // We will fetch all
      return ExecutionState::DONE;
    }
    if (getUpstreamState() == ExecutorState::DONE) {
      return ExecutionState::DONE;
    }
    return ExecutionState::HASMORE;
  }

  auto getSkip() -> size_t {
    size_t skip = 0;
    size_t available = getInput();
    auto call = getCall();
    if (call.getOffset() > 0) {
      skip = std::min(skip, available);
      available -= skip;
    }
    if (call.hasHardLimit() && call.needsFullCount()) {
      // Take away the rows that will be produced
      // add the leftOver to skip
      available -= std::min(available, call.getLimit());
      skip += available;
    }
    return skip;
  }

  auto getOutput() -> MatrixBuilder<1> {
    MatrixBuilder<1> res;
    auto call = getCall();
    auto available = std::min(getInput(), call.getOffset() + call.getLimit());
    for (auto i = call.getOffset(); i < available; ++i) {
      res.emplace_back(RowBuilder<1>{static_cast<int>(i)});
    }
    return res;
  }

  auto getStats() -> ExecutionStats {
    ExecutionStats stats;

    if (doCount()) {
      auto available = getInput();
      auto call = getCall();
      available -= std::min(available, call.getOffset());
      available = std::min(available, call.getLimit());
      stats.count = available;
    }

    return stats;
  }

  RegIdSet const toWrite = {};
  RegIdFlatSetStack const toKeep = {{0}};
  RegIdFlatSet const toClear = {};

  auto prepareOutputRow(SharedAqlItemBlockPtr input) -> OutputAqlItemRow {
    auto const& [unused, upstreamState, clientCall, unused2] = GetParam();
    AqlCall callCopy = clientCall;
    // For passthrough we reuse the block
    return OutputAqlItemRow(input, toWrite, toKeep, toClear, callCopy,
                            OutputAqlItemRow::CopyRowBehavior::DoNotCopyInputRows);
  }
};

TEST_P(IdExecutorTestCombiner, test_produce_datarange_constFetcher) {
  auto input = getInput();
  auto upstreamState = getUpstreamState();
  auto clientCall = getCall();
  auto inputRange = prepareInputRange();
  auto outputRow = prepareOutputRow(inputRange.getBlock());

  // If the input is empty, all rows(none) are used, otherwise they are not.
  EXPECT_EQ(outputRow.allRowsUsed(), input == 0);
  IdExecutorInfos infos{1, 0, "", doCount()};
  std::shared_ptr<VPackBuilder> fakeFetcherInput{VPackParser::fromJson("[ ]")};
  ConstFetcher cFetcher = ConstFetcherHelper{manager(), fakeFetcherInput->buffer()};
  IdExecutor<ConstFetcher> testee{cFetcher, infos};

  auto const [state, stats, call] = testee.produceRows(inputRange, outputRow);
  EXPECT_EQ(state, upstreamState);
  // Stats are NoStats, no checks here.

  // We can never forward any offset.
  EXPECT_EQ(call.getOffset(), 0);

  // The limits need to be reduced by input size.
  EXPECT_EQ(call.softLimit + input, clientCall.softLimit);
  EXPECT_EQ(call.hardLimit + input, clientCall.hardLimit);

  // We can forward fullCount if it is there.
  EXPECT_EQ(call.needsFullCount(), clientCall.needsFullCount());

  // This internally actually asserts that all input rows are "copied".
  EXPECT_TRUE(outputRow.allRowsUsed());
  auto result = outputRow.stealBlock();
  if (input > 0) {
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->size(), input);
    for (size_t i = 0; i < input; ++i) {
      auto val = result->getValueReference(i, 0);
      ASSERT_TRUE(val.isNumber());
      EXPECT_EQ(val.toInt64(), i);
    }
  } else {
    EXPECT_EQ(result, nullptr);
  }
}

TEST_P(IdExecutorTestCombiner, test_produce_datarange_singleRowFetcher) {
  makeExecutorTestHelper<1, 1>()
      .addConsumer<IdExecutor<SingleRowFetcher<::arangodb::aql::BlockPassthrough::Enable>>>(
          makeRegisterInfos(), makeExecutorInfos(), ExecutionNode::SINGLETON)
      .setInputFromRowNum(getInput())
      .setCall(getCall())
      .expectedState(getExpectedState())
      .expectSkipped(getSkip())
      .expectOutput({0}, getOutput())
      .expectedStats(getStats())
      .run();
}

/**
 * In order to test this executor
 * Only the following input cases are relevant:
 * 1) Empty input and Done
 * 2) Empty input and HasMore
 * 3) Input with data and done
 * 4) Input with data and HasMore
 *
 * And only the following Call cases are relevant:
 * 1) Call limit > data, fullCount: false
 * 2) Call limit > data, fullCount: true
 * 3) Call limit == data, fullCount: false
 * 4) Call limit == data, fullCount: true
 * 5) Unlimited call
 *
 * All other cases are excluded by Passhtrough.
 *
 * This executor is templated by two fetcher types:
 *   ConstFetcher
 *   SingleRowFetcher<passthrough>
 *
 * The output row has the following copy types
 *   DoNotCopy << This is actually used in production, however we cannot test that we actually do something with it
 *   DoCopy  << This is to assert that copying is performaed
 */

static auto inputs = testing::Values(0,  // Test empty input
                                     3   // Test input data
);

auto upstreamStates = testing::Values(ExecutorState::HASMORE, ExecutorState::DONE);
auto clientCalls = testing::Values(AqlCall{},  // unlimited call
                                   AqlCall{0, 3u, AqlCall::Infinity{}, false},  // softlimit call (note this is equal to length of input data)
                                   AqlCall{0, AqlCall::Infinity{}, 3u, false},  // hardlimit call (note this is equal to length of input data), no fullcount
                                   AqlCall{0, AqlCall::Infinity{}, 3u, true},  // hardlimit call (note this is equal to length of input data), with fullcount
                                   AqlCall{0, 7u, AqlCall::Infinity{}, false},  // softlimit call (note this is larger than length of input data)
                                   AqlCall{0, AqlCall::Infinity{}, 7u, false},  // hardlimit call (note this is larger than length of input data), no fullcount
                                   AqlCall{0, AqlCall::Infinity{}, 7u, true}  // hardlimit call (note this is larger than length of input data), with fullcount
);

INSTANTIATE_TEST_CASE_P(IdExecutorTest, IdExecutorTestCombiner,
                        ::testing::Combine(inputs, upstreamStates, clientCalls,
                                           ::testing::Bool()));

class IdExecutionBlockTest : public AqlExecutorTestCase<> {};

// The IdExecutor has a specific initializeCursor method in ExecutionBlockImpl
TEST_F(IdExecutionBlockTest, test_initialize_cursor_get) {
  RegisterInfos registerInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
  IdExecutorInfos executorInfos{false};
  ExecutionBlockImpl<IdExecutor<ConstFetcher>> testee{fakedQuery->rootEngine(),
                                                      generateNodeDummy(),
                                                      std::move(registerInfos),
                                                      std::move(executorInfos)};
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});

  for (size_t i = 0; i < inputBlock->size(); ++i) {
    InputAqlItemRow input{inputBlock, i};
    ASSERT_TRUE(input.isInitialized());
    {
      // Test first call, executor is done, cannot skip and does not return
      AqlCall call{};
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      EXPECT_EQ(block, nullptr);
    }
    {
      // Initialize cursor
      auto const& [state, result] = testee.initializeCursor(input);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_TRUE(result.ok());
    }
    {
      // Test second call, executor needs to return the row
      AqlCall call{};
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      ASSERT_NE(block, nullptr);
      EXPECT_EQ(block->size(), 1);
      auto const& val = block->getValueReference(0, 0);
      ASSERT_TRUE(val.isNumber());
      EXPECT_EQ(static_cast<size_t>(val.toInt64()), i);
    }
  }
}

// The IdExecutor has a specific initializeCursor method in ExecutionBlockImpl
TEST_F(IdExecutionBlockTest, test_initialize_cursor_skip) {
  RegisterInfos registerInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
  IdExecutorInfos executorInfos{false};
  ExecutionBlockImpl<IdExecutor<ConstFetcher>> testee{fakedQuery->rootEngine(),
                                                      generateNodeDummy(),
                                                      std::move(registerInfos),
                                                      std::move(executorInfos)};
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});

  for (size_t i = 0; i < inputBlock->size(); ++i) {
    InputAqlItemRow input{inputBlock, i};
    ASSERT_TRUE(input.isInitialized());
    {
      // Test first call, executor is done, cannot skip and does not return
      AqlCall call{};
      call.offset = 10;
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      EXPECT_EQ(block, nullptr);
    }
    {
      // Initialize cursor
      auto const& [state, result] = testee.initializeCursor(input);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_TRUE(result.ok());
    }
    {
      // Test second call, executor needs to skip the row
      AqlCall call{};
      call.offset = 10;
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 1);
      ASSERT_EQ(block, nullptr);
    }
  }
}

// The IdExecutor has a specific initializeCursor method in ExecutionBlockImpl
TEST_F(IdExecutionBlockTest, test_initialize_cursor_fullCount) {
  RegisterInfos registerInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
  IdExecutorInfos executorInfos{false};
  ExecutionBlockImpl<IdExecutor<ConstFetcher>> testee{fakedQuery->rootEngine(),
                                                      generateNodeDummy(),
                                                      std::move(registerInfos),
                                                      std::move(executorInfos)};
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});

  for (size_t i = 0; i < inputBlock->size(); ++i) {
    InputAqlItemRow input{inputBlock, i};
    ASSERT_TRUE(input.isInitialized());
    {
      // Test first call, executor is done, cannot skip and does not return
      AqlCall call{};
      call.hardLimit = 0u;
      call.fullCount = true;
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      EXPECT_EQ(block, nullptr);
    }
    {
      // Initialize cursor
      auto const& [state, result] = testee.initializeCursor(input);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_TRUE(result.ok());
    }
    {
      // Test second call, executor needs to skip the row
      AqlCall call{};
      call.hardLimit = 0u;
      call.fullCount = true;
      auto const& [state, skipped, block] =
          testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 1);
      ASSERT_EQ(block, nullptr);
    }
  }
}

TEST_F(IdExecutionBlockTest, test_hardlimit_single_row_fetcher) {
  RegisterInfos registerInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
  IdExecutorInfos executorInfos{false};
  makeExecutorTestHelper()
      .addConsumer<IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>(std::move(registerInfos), std::move(executorInfos))
      .setInputValueList(1, 2, 3, 4, 5, 6)
      .setCall(AqlCall{0, AqlCall::Infinity{}, 2u, false})
      .expectOutput({0}, {{1}, {2}})
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .run();
}

/**
 * @brief This are special tests, the ConstFetcher is overloaded
 *  with data rows, now the IdExecutor which is passthrough
 *  asks for a much lower hard limit.
 *  Used in ScatterExecutor logic.
 *  param: useFullCount
 */
class BlockOverloadTest : public AqlExecutorTestCaseWithParam<bool> {
 protected:
  auto getTestee() -> ExecutionBlockImpl<IdExecutor<ConstFetcher>> {
    RegisterInfos registerInfos{{}, {}, 1, 1, {}, {RegIdSet{0}}};
    IdExecutorInfos executorInfos{false};
    return ExecutionBlockImpl<IdExecutor<ConstFetcher>>{fakedQuery->rootEngine(),
                                                        generateNodeDummy(),
                                                        std::move(registerInfos),
                                                        std::move(executorInfos)};
  }

  auto useFullCount() -> bool { return GetParam(); }
};

INSTANTIATE_TEST_CASE_P(IdExecutionBlock, BlockOverloadTest, ::testing::Bool());

TEST_P(BlockOverloadTest, test_hardlimit_const_fetcher) {
  auto testee = getTestee();
  {
    // Inject block
    auto inputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}, {6}});

    testee.injectConstantBlock(inputBlock, SkipResult{});
  }
  {
    // Now call with too small hardLimit
    auto expectedOutputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});
    AqlCall call{};
    call.hardLimit = 3u;
    call.fullCount = useFullCount();
    auto const& [state, skipped, block] = testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    if (useFullCount()) {
      EXPECT_EQ(skipped.getSkipCount(), 4);
    } else {
      EXPECT_EQ(skipped.getSkipCount(), 0);
    }

    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Validate that additional upstream-rows are gone.
    AqlCall call{};
    auto const& [state, skipped, block] = testee.execute(AqlCallStack::emptyStack(), AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    EXPECT_EQ(block, nullptr);
  }
}

TEST_P(BlockOverloadTest, test_hardlimit_const_fetcher_shadow_rows_at_end) {
  auto testee = getTestee();
  {
    // Inject block
    auto inputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}, {6}},
                      {{5, 0}, {6, 1}});
    SkipResult skipRes{};
    // we have depth 0 and depth 1 shadowRow. Adjust skipResult for it
    skipRes.incrementSubquery();
    skipRes.incrementSubquery();
    testee.injectConstantBlock(inputBlock, skipRes);
  }
  {
    // Now call with too small hardLimit
    auto expectedOutputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {5}, {6}}, {{3, 0}, {4, 1}});
    AqlCall call{};
    call.hardLimit = 3u;
    call.fullCount = useFullCount();
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});

    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    if (useFullCount()) {
      EXPECT_EQ(skipped.getSkipCount(), 2);
    } else {
      EXPECT_EQ(skipped.getSkipCount(), 0);
    }
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Validate that additional upstream-rows are gone.
    AqlCall call{};
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    EXPECT_EQ(block, nullptr);
  }
}

TEST_P(BlockOverloadTest, test_hardlimit_const_fetcher_shadow_rows_in_between) {
  auto testee = getTestee();
  {
    // Inject block
    auto inputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}, {6}},
                      {{3, 0}, {4, 1}, {6, 0}});
    SkipResult skipRes{};
    // we have depth 0 and depth 1 shadowRow. Adjust skipResult for it
    skipRes.incrementSubquery();
    skipRes.incrementSubquery();
    testee.injectConstantBlock(inputBlock, skipRes);
  }
  {
    // Now call with too small hardLimit
    auto expectedOutputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {3}, {4}}, {{2, 0}, {3, 1}});
    AqlCall call{};
    call.hardLimit = 2u;
    call.fullCount = useFullCount();
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::HASMORE);
    if (useFullCount()) {
      EXPECT_EQ(skipped.getSkipCount(), 1);
    } else {
      EXPECT_EQ(skipped.getSkipCount(), 0);
    }
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Validate that next call will give remaining rows
    // Now call with too small hardLimit
    auto expectedOutputBlock = buildBlock<1>(itemBlockManager, {{5}, {6}}, {{1, 0}});
    AqlCall call{};
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
}

// Consecutive ShadowRows can be returned within the same block.
TEST_P(BlockOverloadTest, test_hardlimit_const_fetcher_consecutive_shadow_rows) {
  auto testee = getTestee();
  {
    // Inject block
    auto inputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}, {6}},
                      {{3, 0}, {4, 1}, {5, 0}, {6, 0}});
    SkipResult skipRes{};
    // we have depth 0 and depth 1 shadowRow. Adjust skipResult for it
    skipRes.incrementSubquery();
    skipRes.incrementSubquery();
    testee.injectConstantBlock(inputBlock, skipRes);
  }
  // We can only return until the next top-level shadow row is reached.
  {
    // Now call with too small hardLimit
    auto expectedOutputBlock =
        buildBlock<1>(itemBlockManager, {{0}, {1}, {3}, {4}}, {{2, 0}, {3, 1}});
    AqlCall call{};
    call.hardLimit = 2u;
    call.fullCount = useFullCount();
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::HASMORE);
    if (useFullCount()) {
      EXPECT_EQ(skipped.getSkipCount(), 1);
    } else {
      EXPECT_EQ(skipped.getSkipCount(), 0);
    }
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Second call will only find a single ShadowRow
    auto expectedOutputBlock = buildBlock<1>(itemBlockManager, {{5}}, {{0, 0}});
    AqlCall call{};
    call.hardLimit = 2u;
    call.fullCount = useFullCount();
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::HASMORE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Third call will only find a single ShadowRow
    auto expectedOutputBlock = buildBlock<1>(itemBlockManager, {{6}}, {{0, 0}});
    AqlCall call{};
    call.hardLimit = 2u;
    call.fullCount = useFullCount();
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    asserthelper::ValidateBlocksAreEqual(block, expectedOutputBlock);
  }
  {
    // Validate that additional upstream-rows are gone.
    AqlCall call{};
    // First put enough fetch all subquery call onto the stack
    AqlCallStack stack;
    stack.pushCall(AqlCallList{AqlCall{}});
    stack.pushCall(AqlCallList{AqlCall{}});
    auto const& [state, skipped, block] = testee.execute(stack, AqlCallList{std::move(call)});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    EXPECT_EQ(block, nullptr);
  }
}

}  // namespace arangodb::tests::aql
