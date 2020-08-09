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

#include "AqlItemBlockHelper.h"
#include "Mocks/Servers.h"
#include "WaitingExecutionBlockMock.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlCallStack.h"
#include "Aql/ClusterNodes.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/Query.h"
#include "Aql/RegisterInfos.h"
#include "Aql/ScatterExecutor.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb::tests::aql {

class SharedScatterExecutionBlockTest {
 protected:
  mocks::MockAqlServer server{};
  ResourceMonitor monitor{};
  AqlItemBlockManager itemBlockManager{&monitor, SerializationFormat::SHADOWROWS};
  std::unique_ptr<arangodb::aql::Query> fakedQuery{server.createFakeQuery()};
  std::vector<std::unique_ptr<ExecutionNode>> _execNodes;
  velocypack::Options vpackOptions;
  std::vector<std::string> clientIds{"a", "b", "c"};

  SharedScatterExecutionBlockTest() {
    auto engine =
      std::make_unique<ExecutionEngine>(*fakedQuery,
                                        itemBlockManager,
                                        SerializationFormat::SHADOWROWS);
    /// TODO fakedQuery->setEngine(engine.release());
  }

  auto buildStack(AqlCall call, size_t subqueryDepth = 0) -> std::pair<AqlCallStack, AqlCallList> {
    AqlCallStack stack;
    for (size_t i = 0; i < subqueryDepth; ++i) {
      // We do never care for details of outer Subqueries.
      // So let them do overfetching as the like
      stack.pushCall(AqlCallList{AqlCall{}, AqlCall{}});
    }
    return {stack, AqlCallList{call}};
  }

  /**
   * @brief Creates and manages a ExecutionNode.
   *        These nodes can be used to create the Executors
   *        Caller does not need to manage the memory.
   *
   * @return ExecutionNode* Pointer to a dummy ExecutionNode. Memory is managed, do not delete.
   */
  auto generateNodeDummy() -> ExecutionNode* {
    auto dummy = std::make_unique<SingletonNode>(
      const_cast<arangodb::aql::ExecutionPlan*>(fakedQuery->plan()),
      ExecutionNodeId{_execNodes.size()});
    auto res = dummy.get();
    _execNodes.emplace_back(std::move(dummy));
    return res;
  }

  auto generateScatterNode() -> ScatterNode* {
    auto dummy = std::make_unique<ScatterNode>(
            const_cast<arangodb::aql::ExecutionPlan*>(fakedQuery->plan()),
            ExecutionNodeId{_execNodes.size()},
            ScatterNode::ScatterType::SHARD);
    auto res = dummy.get();
    _execNodes.emplace_back(std::move(dummy));
    return res;
  }

  auto generateRegisterInfos() const -> RegisterInfos {
    return RegisterInfos{RegIdSet{0}, {}, 1, 1, {}, {RegIdSet{0}}};
  }

  auto generateExecutorInfos() const -> ScatterExecutorInfos {
    return ScatterExecutorInfos{clientIds};
  }

  auto createProducer(SharedAqlItemBlockPtr inputBlock, size_t subqueryDepth = 0)
      -> WaitingExecutionBlockMock {
    std::deque<SharedAqlItemBlockPtr> blockDeque;
    // TODO add input splicing
    blockDeque.push_back(inputBlock);
    return createProducer(blockDeque, subqueryDepth);
  }

  auto createProducer(std::deque<SharedAqlItemBlockPtr> blockDeque,
                      size_t subqueryDepth = 0) -> WaitingExecutionBlockMock {
    // TODO add input splicing

    return WaitingExecutionBlockMock{fakedQuery->rootEngine(), generateNodeDummy(),
                                     std::move(blockDeque),
                                     WaitingExecutionBlockMock::WaitingBehaviour::NEVER,
                                     subqueryDepth};
  }

  auto ValidateBlocksAreEqual(SharedAqlItemBlockPtr actual, SharedAqlItemBlockPtr expected) {
    ASSERT_NE(expected, nullptr);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->size(), expected->size());
    EXPECT_EQ(actual->getNrRegs(), 1);
    for (size_t i = 0; i < (std::min)(actual->size(), expected->size()); ++i) {
      if (actual->isShadowRow(i)) {
        ASSERT_TRUE(expected->isShadowRow(i))
            << "Row " << i << " is not supposed to be a shadow row.";
      } else {
        EXPECT_FALSE(expected->isShadowRow(i))
            << "Row " << i << " is supposed to be a shadow row.";
        auto const& x = actual->getValueReference(i, 0);
        auto const& y = expected->getValueReference(i, 0);
        EXPECT_TRUE(AqlValue::Compare(&vpackOptions, x, y, true) == 0)
            << "Row " << i << " Column " << 0 << " do not agree. "
            << x.slice().toJson(&vpackOptions) << " vs. "
            << y.slice().toJson(&vpackOptions);
      }
    }
  }
};

// The tests of this suite test all permutations of client calls.
// This way we can ensure that the block works even on parallel
// execution.
class RandomOrderTest : public SharedScatterExecutionBlockTest,
                        public ::testing::TestWithParam<std::vector<std::string>> {
 protected:
  std::vector<std::string> const& getCallOrder() { return GetParam(); }

  RandomOrderTest() {}
};

namespace {
template <typename T>
auto ArrayPermutations(std::vector<T> base) -> std::vector<std::vector<T>> {
  std::vector<std::vector<T>> res;
  // This is not corect we would need faculity of base, but we are in a test...
  res.reserve(base.size());
  do {
    res.emplace_back(base);
  } while (std::next_permutation(base.begin(), base.end()));
  return res;
};

auto randomOrderCalls = ArrayPermutations<std::string>({"a", "b", "c"});
}  // namespace

INSTANTIATE_TEST_CASE_P(ScatterExecutionBlockTestRandomOrder, RandomOrderTest,
                        ::testing::ValuesIn(randomOrderCalls));

TEST_P(RandomOrderTest, all_clients_should_get_the_block) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});
  auto producer = createProducer(inputBlock);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client);
    AqlCall call{};  // DefaultCall
    auto [stack, clientCall] = buildStack(call);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, inputBlock);
  }
}

TEST_P(RandomOrderTest, all_clients_can_skip_the_block) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});
  auto producer = createProducer(inputBlock);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client);
    AqlCall call{};
    call.offset = 10;
    auto [stack, clientCall] = buildStack(call);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 3);
    EXPECT_EQ(block, nullptr);
  }
}

TEST_P(RandomOrderTest, all_clients_can_fullcount_the_block) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});
  auto expectedBlock = buildBlock<1>(itemBlockManager, {{0}});
  auto producer = createProducer(inputBlock);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client);
    AqlCall call{};
    call.hardLimit = 1u;
    call.fullCount = true;
    auto [stack, clientCall] = buildStack(call);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 2);
    ValidateBlocksAreEqual(block, expectedBlock);
  }
}

TEST_P(RandomOrderTest, all_clients_can_have_different_calls) {
  auto inputBlock =
      buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}, {6}});
  auto producer = createProducer(inputBlock);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client);
    if (client == "a") {
      // Just produce all
      AqlCall call{};
      auto [stack, clientCall] = buildStack(call);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      ValidateBlocksAreEqual(block, inputBlock);
    } else if (client == "b") {
      AqlCall call{};
      call.offset = 2;
      call.hardLimit = 2u;
      auto [stack, clientCall] = buildStack(call);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 2);
      auto expectedBlock = buildBlock<1>(itemBlockManager, {{2}, {3}});
      ValidateBlocksAreEqual(block, expectedBlock);
    } else if (client == "c") {
      {
        AqlCall call{};
        call.softLimit = 2u;
        auto [stack, clientCall] = buildStack(call);
        auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
        EXPECT_EQ(state, ExecutionState::HASMORE);
        EXPECT_EQ(skipped.getSkipCount(), 0);
        auto expectedBlock = buildBlock<1>(itemBlockManager, {{0}, {1}});
        ValidateBlocksAreEqual(block, expectedBlock);
      }
      {
        // As we have softLimit we can simply call again
        AqlCall call{};
        call.offset = 1;
        call.softLimit = 2u;
        auto [stack, clientCall] = buildStack(call);
        auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
        EXPECT_EQ(state, ExecutionState::HASMORE);
        EXPECT_EQ(skipped.getSkipCount(), 1);
        auto expectedBlock = buildBlock<1>(itemBlockManager, {{3}, {4}});
        ValidateBlocksAreEqual(block, expectedBlock);
      }
    }
  }
}

TEST_P(RandomOrderTest, get_does_not_jump_over_shadowrows) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}},
                                  {{3, 0}, {5, 0}});
  auto firstExpectedBlock =
      buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}}, {{3, 0}});
  auto secondExpectedBlock = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{1, 0}});
  size_t subqueryDepth = 1;
  auto producer = createProducer(inputBlock, subqueryDepth);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  // First call. reach first shadowrow, but do not jump over it, we do not know
  // how to proceed after (e.g. skip the rows).
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " first call");
    // Produce all until shadow row
    AqlCall call{};
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::HASMORE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, firstExpectedBlock);
  }

  // Second call. reach up to last shadowRow and figure out that we are essentially done
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " second call");
    // Produce all until shadow row
    AqlCall call{};
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, secondExpectedBlock);
  }
}

TEST_P(RandomOrderTest, handling_of_higher_depth_shadowrows_produce) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}},
                                  {{2, 0}, {3, 1}, {5, 0}});
  auto firstExpectedBlock =
      buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}}, {{2, 0}, {3, 1}});
  auto secondExpectedBlock = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{1, 0}});

  size_t subqueryDepth = 2;
  auto producer = createProducer(inputBlock, subqueryDepth);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  // First call. reach first shadowrow, but do not jump over it, we do not know
  // how to proceed after (e.g. skip the rows).
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " first call");
    // Produce all until shadow row
    AqlCall call{};
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::HASMORE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, firstExpectedBlock);
  }

  // Second call. reach up to last shadowRow and figure out that we are essentially done
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " second call");
    // Produce all until shadow row
    AqlCall call{};
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, secondExpectedBlock);
  }
}

TEST_P(RandomOrderTest, handling_of_higher_depth_shadowrows_skip) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}},
                                  {{2, 0}, {3, 1}, {5, 0}});
  auto firstExpectedBlock =
      buildBlock<1>(itemBlockManager, {{2}, {3}}, {{0, 0}, {1, 1}});
  auto secondExpectedBlock = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{1, 0}});
  size_t subqueryDepth = 2;
  auto producer = createProducer(inputBlock, subqueryDepth);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  // First call. reach first shadowrow, but do not jump over it, we do not know
  // how to proceed after (e.g. skip the rows).
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " first call");
    // Produce all until shadow row
    AqlCall call{};
    call.offset = 10;
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::HASMORE);
    EXPECT_EQ(skipped.getSkipCount(), 2);
    ValidateBlocksAreEqual(block, firstExpectedBlock);
  }

  // Second call. reach up to last shadowRow and figure out that we are essentially done
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " second call");
    // Produce all until shadow row
    AqlCall call{};
    auto [stack, clientCall] = buildStack(call, subqueryDepth);
    auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ValidateBlocksAreEqual(block, secondExpectedBlock);
  }
}

TEST_P(RandomOrderTest, handling_of_consecutive_shadow_rows) {
  // As there is no produce inbetween we are actually able to just forward it
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}},
                                  {{2, 0}, {3, 1}, {4, 0}, {5, 1}});
  size_t subqueryDepth = 2;
  auto producer = createProducer(inputBlock, subqueryDepth);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  // First call. actually there are only shadowRows following, we would be able
  // to plainly forward everything, however this is not suppoert yet
  // so we need to ask once for every relevant shadow row (depth 0)
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client);
    {
      // Produce all until second relevant shadow row
      AqlCall call{};
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::HASMORE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      auto expected =
          buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}}, {{2, 0}, {3, 1}});
      ValidateBlocksAreEqual(block, expected);
    }
    {
      // Produce the last shadow rows
      AqlCall call{};
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      auto expected = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{0, 0}, {1, 1}});
      ValidateBlocksAreEqual(block, expected);
    }
  }
}

TEST_P(RandomOrderTest, shadowrows_with_different_call_types) {
  auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}, {4}, {5}},
                                  {{3, 0}, {5, 0}});
  size_t subqueryDepth = 1;
  auto producer = createProducer(inputBlock, subqueryDepth);

  ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                             generateScatterNode(),
                                             generateRegisterInfos(),
                                             generateExecutorInfos()};
  testee.addDependency(&producer);

  // First call. desired to be stopped at shadowRow
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " first call.");
    if (client == "a") {
      // Just produce all
      AqlCall call{};
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::HASMORE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      auto expectedBlock =
          buildBlock<1>(itemBlockManager, {{0}, {1}, {2}, {3}}, {{3, 0}});
      ValidateBlocksAreEqual(block, expectedBlock);
    } else if (client == "b") {
      AqlCall call{};
      call.offset = 2;
      call.hardLimit = 2u;
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::HASMORE);
      EXPECT_EQ(skipped.getSkipCount(), 2);
      auto expectedBlock = buildBlock<1>(itemBlockManager, {{2}, {3}}, {{1, 0}});
      ValidateBlocksAreEqual(block, expectedBlock);
    } else if (client == "c") {
      {
        AqlCall call{};
        call.softLimit = 2u;
        auto [stack, clientCall] = buildStack(call, subqueryDepth);
        auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
        EXPECT_EQ(state, ExecutionState::HASMORE);
        EXPECT_EQ(skipped.getSkipCount(), 0);
        auto expectedBlock = buildBlock<1>(itemBlockManager, {{0}, {1}});
        ValidateBlocksAreEqual(block, expectedBlock);
      }
      {
        // As we have softLimit we can simply call again
        AqlCall call{};
        call.offset = 1;
        call.softLimit = 2u;
        auto [stack, clientCall] = buildStack(call, subqueryDepth);
        auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
        EXPECT_EQ(state, ExecutionState::HASMORE);
        EXPECT_EQ(skipped.getSkipCount(), 1);
        auto expectedBlock = buildBlock<1>(itemBlockManager, {{3}}, {{0, 0}});
        ValidateBlocksAreEqual(block, expectedBlock);
      }
    }
  }

  // Second call. desired to be stopped at shadowRow
  for (auto const& client : getCallOrder()) {
    SCOPED_TRACE("Testing client " + client + " second call.");
    if (client == "a") {
      // Just produce all
      AqlCall call{};
      call.hardLimit = 1u;
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      auto expectedBlock = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{1, 0}});
      ValidateBlocksAreEqual(block, expectedBlock);
    } else if (client == "b") {
      AqlCall call{};
      call.softLimit = 1u;
      auto [stack, clientCall] = buildStack(call, subqueryDepth);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      EXPECT_EQ(state, ExecutionState::DONE);
      EXPECT_EQ(skipped.getSkipCount(), 0);
      auto expectedBlock = buildBlock<1>(itemBlockManager, {{4}, {5}}, {{1, 0}});
      ValidateBlocksAreEqual(block, expectedBlock);
    } else if (client == "c") {
      {
        AqlCall call{};
        call.offset = 10;
        auto [stack, clientCall] = buildStack(call, subqueryDepth);
        auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
        EXPECT_EQ(state, ExecutionState::DONE);
        EXPECT_EQ(skipped.getSkipCount(), 1);
        auto expectedBlock = buildBlock<1>(itemBlockManager, {{5}}, {{0, 0}});
        ValidateBlocksAreEqual(block, expectedBlock);
      }
    }
  }
}

// This test does not include randomization of clientCall ordering
class ScatterExecutionBlockTest : public SharedScatterExecutionBlockTest,
                                  public ::testing::Test {};

// Here we do a more specific ordering of calls, as we need to rearange multidepthCalls

TEST_F(ScatterExecutionBlockTest, any_ordering_of_calls_is_fine) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  std::unordered_map<std::string, std::pair<size_t, std::vector<SharedAqlItemBlockPtr>>> expected;
  std::vector<std::string> callOrder;
  for (auto const& c : clientIds) {
    expected[c] = std::make_pair(0, std::vector<SharedAqlItemBlockPtr>{});
  }

  {
    auto inputBlock = buildBlock<1>(itemBlockManager, {{0}, {1}, {2}});
    blockDeque.emplace_back(inputBlock);
    for (auto const& c : clientIds) {
      expected[c].second.emplace_back(inputBlock);
      callOrder.emplace_back(c);
    }
  }

  {
    auto inputBlock = buildBlock<1>(itemBlockManager, {{3}, {4}, {5}, {6}});
    blockDeque.emplace_back(inputBlock);
    for (auto const& c : clientIds) {
      expected[c].second.emplace_back(inputBlock);
      callOrder.emplace_back(c);
    }
  }

  {
    auto inputBlock = buildBlock<1>(itemBlockManager, {{7}, {8}, {9}});
    blockDeque.emplace_back(inputBlock);
    for (auto const& c : clientIds) {
      expected[c].second.emplace_back(inputBlock);
      callOrder.emplace_back(c);
    }
  }
  // Every client will ask every block alone.
  ASSERT_EQ(callOrder.size(), clientIds.size() * blockDeque.size());
  // Now we do all permuation of potentiall call ordering
  do {
    auto producer = createProducer(blockDeque);
    ExecutionBlockImpl<ScatterExecutor> testee{fakedQuery->rootEngine(),
                                               generateScatterNode(),
                                               generateRegisterInfos(),
                                               generateExecutorInfos()};
    testee.addDependency(&producer);
    for (auto& [c, pair] : expected) {
      // Reset seen position
      pair.first = 0;
    }
    std::stringstream permutation;
    for (auto c : callOrder) {
      permutation << " " << c;
    }
    SCOPED_TRACE("Testing permutation: " + permutation.str());
    for (auto const& client : callOrder) {
      auto& [callNr, blocks] = expected[client];
      SCOPED_TRACE("Testing client " + client + " call number " + std::to_string(callNr));
      AqlCall call{};
      auto [stack, clientCall] = buildStack(call);
      auto const [state, skipped, block] = testee.executeForClient(stack, clientCall, client);
      if (callNr == 2) {
        EXPECT_EQ(state, ExecutionState::DONE);
      } else {
        EXPECT_EQ(state, ExecutionState::HASMORE);
      }
      EXPECT_EQ(skipped.getSkipCount(), 0);
      ASSERT_TRUE(callNr < blocks.size());
      ValidateBlocksAreEqual(block, blocks[callNr]);
      callNr++;
    }
  } while (std::next_permutation(callOrder.begin(), callOrder.end()));
}

// TODO add test for initilaize cursor

}  // namespace arangodb::tests::aql
