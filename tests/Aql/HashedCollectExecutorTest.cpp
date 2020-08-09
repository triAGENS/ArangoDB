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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "AqlExecutorTestCase.h"
#include "AqlItemBlockHelper.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/HashedCollectExecutor.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Mocks/Servers.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>
#include <functional>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

// This is only to get a split-type. The Type is independent of actual template parameters
using HashedCollectTestHelper = ExecutorTestHelper<1, 1>;
using HashedCollectSplitType = HashedCollectTestHelper::SplitType;
using HashedCollectInputParam = std::tuple<HashedCollectSplitType, bool>;

class HashedCollectExecutorTest
    : public AqlExecutorTestCaseWithParam<HashedCollectInputParam> {
 protected:
  auto getSplit() -> HashedCollectSplitType {
    auto [split, empty] = GetParam();
    return split;
  }

  auto appendEmpty() -> bool {
    auto [split, empty] = GetParam();
    return empty;
  }

  auto buildRegisterInfos(RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                          std::vector<std::pair<RegisterId, RegisterId>> groupRegisters,
                          RegisterId collectRegister = RegisterPlan::MaxRegisterId,
                          std::vector<std::pair<RegisterId, RegisterId>> aggregateRegisters = {})
      -> RegisterInfos {
    RegIdSet registersToClear{};
    RegIdSetStack registersToKeep{{}};
    auto readableInputRegisters = RegIdSet{};
    auto writeableOutputRegisters = RegIdSet{};

    for (RegisterId i = 0; i < nrInputRegisters; ++i) {
      // All registers need to be invalidated!
      registersToClear.emplace(i);
    }

    for (auto const& [out, in] : groupRegisters) {
      readableInputRegisters.emplace(in);
      writeableOutputRegisters.emplace(out);
    }

    if (collectRegister != RegisterPlan::MaxRegisterId) {
      writeableOutputRegisters.emplace(collectRegister);
    }
    for (auto const& [out, in] : aggregateRegisters) {
      if (in != RegisterPlan::MaxRegisterId) {
        readableInputRegisters.emplace(in);
      }
      writeableOutputRegisters.emplace(out);
    }

    return RegisterInfos{std::move(readableInputRegisters),
                         std::move(writeableOutputRegisters),
                         nrInputRegisters,
                         nrOutputRegisters,
                         registersToClear,
                         registersToKeep};
  };

  auto buildExecutorInfos(RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                          std::vector<std::pair<RegisterId, RegisterId>> groupRegisters,
                          RegisterId collectRegister = RegisterPlan::MaxRegisterId,
                          std::vector<std::string> aggregateTypes = {},
                          std::vector<std::pair<RegisterId, RegisterId>> aggregateRegisters = {})
      -> HashedCollectExecutorInfos {
    // It seems that count <=> collectRegister exists
    bool count = false;
    if (collectRegister != RegisterPlan::MaxRegisterId) {
      count = true;
    }

    return HashedCollectExecutorInfos{std::move(groupRegisters),
                                      collectRegister,
                                      std::move(aggregateTypes),
                                      std::move(aggregateRegisters),
                                      &VPackOptions::Defaults,
                                      count};
  };
};

template <size_t... vs>
const HashedCollectSplitType splitIntoBlocks =
    HashedCollectSplitType{std::vector<std::size_t>{vs...}};
template <size_t step>
const HashedCollectSplitType splitStep = HashedCollectSplitType{step};

INSTANTIATE_TEST_CASE_P(HashedCollect, HashedCollectExecutorTest,
                        ::testing::Combine(::testing::Values(splitIntoBlocks<2, 3>,
                                                             splitIntoBlocks<3, 4>,
                                                             splitStep<1>, splitStep<2>),
                                           ::testing::Bool()));

// Collect with only one group value
TEST_P(HashedCollectExecutorTest, collect_only) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect skip all
TEST_P(HashedCollectExecutorTest, skip_all) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.offset = 1000;      // skip all
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {})
      .allowAnyOutputOrder(true)
      .expectSkipped(4)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect fullCount all
TEST_P(HashedCollectExecutorTest, fullcount_all) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.hardLimit = 0u;     // HardLimit
  call.fullCount = true;   // count all
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {})
      .allowAnyOutputOrder(true)
      .expectSkipped(4)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect get some
TEST_P(HashedCollectExecutorTest, collect_only_soft_less) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.softLimit = 2u;
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true, 2)
      .expectSkipped(0)
      .expectedState(ExecutionState::HASMORE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect get some multiple calls
TEST_P(HashedCollectExecutorTest, collect_only_soft_less_second_call) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});

  // TODO maybe we should add this to the test framework as well.

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  {
    auto data =
        buildBlock<2>(manager(), {{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}});
    blockDeque.emplace_back(data);
  }

  auto inputBlock = std::make_unique<WaitingExecutionBlockMock>(
      fakedQuery->rootEngine(), generateNodeDummy(), std::move(blockDeque),
      WaitingExecutionBlockMock::WaitingBehaviour::NEVER);
  ExecutionBlockImpl<HashedCollectExecutor> testee{fakedQuery->rootEngine(),
                                                   generateNodeDummy(),
                                                   std::move(registerInfos),
                                                   std::move(executorInfos)};
  testee.addDependency(inputBlock.get());
  // First Call
  std::unordered_set<std::size_t> matchedRows{};
  auto buildExpectedOutput = [&]() -> SharedAqlItemBlockPtr {
    MatrixBuilder<1> matrix;
    if (matchedRows.find(0) == matchedRows.end()) {
      matrix.emplace_back(RowBuilder<1>{1});
    }
    if (matchedRows.find(1) == matchedRows.end()) {
      matrix.emplace_back(RowBuilder<1>{2});
    }
    if (matchedRows.find(2) == matchedRows.end()) {
      matrix.emplace_back(RowBuilder<1>{6});
    }
    if (matchedRows.find(3) == matchedRows.end()) {
      matrix.emplace_back(RowBuilder<1>{R"("1")"});
    }
    return buildBlock<1>(manager(), std::move(matrix));
  };
  std::vector<RegisterId> registersToTest{1};
  {
    AqlCall call{};
    call.softLimit = 2u;
    auto const [state, skipped, result] = testee.execute(AqlCallStack::emptyStack(), AqlCallList{call});
    EXPECT_EQ(state, ExecutionState::HASMORE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ASSERT_NE(result, nullptr);
    asserthelper::ValidateBlocksAreEqualUnordered(result, buildExpectedOutput(),
                                                  matchedRows, 2, registersToTest);
  }

  // Second call
  {
    AqlCall call{};
    call.softLimit = 2u;
    auto const [state, skipped, result] = testee.execute(AqlCallStack::emptyStack(), AqlCallList{call});
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_EQ(skipped.getSkipCount(), 0);
    ASSERT_NE(result, nullptr);
    asserthelper::ValidateBlocksAreEqualUnordered(result, buildExpectedOutput(),
                                                  matchedRows, 0, registersToTest);
  }
}

// Collect get some
TEST_P(HashedCollectExecutorTest, collect_only_hard_less) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.hardLimit = 2u;
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true, 2)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect skip some
TEST_P(HashedCollectExecutorTest, skip_some) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.offset = 2;         // skip some
  call.softLimit = 0u;      // 0 limit
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {})
      .allowAnyOutputOrder(true)
      .expectSkipped(2)
      .expectedState(ExecutionState::HASMORE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect skip and get
TEST_P(HashedCollectExecutorTest, skip_and_get) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.offset = 2;         // skip some
  call.softLimit = 1000u;   // high limit
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true, 2)
      .expectSkipped(2)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect skip and hardLimit
TEST_P(HashedCollectExecutorTest, skip_and_hardLimit) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.offset = 2;         // skip some
  call.hardLimit = 1u;     // hard limit
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true, 3)
      .expectSkipped(2)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect skip and fullCount
TEST_P(HashedCollectExecutorTest, skip_and_fullCount) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};
  call.offset = 1;     // skip some
  call.hardLimit = 2u; // hard limit
  call.fullCount = true;
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {2}, {6}, {R"("1")"}})
      .allowAnyOutputOrder(true, 2)
      .expectSkipped(2)
      .expectedState(ExecutionState::DONE)
      .appendEmptyBlock(appendEmpty())
      // .expectedStats(stats)
      .run();
}

// Collect with more then one group value
TEST_P(HashedCollectExecutorTest, collect_only_multiple_values) {
  auto registerInfos = buildRegisterInfos(2, 4, {{2, 0}, {3, 1}});
  auto executorInfos = buildExecutorInfos(2, 4, {{2, 0}, {3, 1}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper<2, 2>()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue(MatrixBuilder<2>{RowBuilder<2>{1, 5}, RowBuilder<2>{1, 1},
                                      RowBuilder<2>{2, 2}, RowBuilder<2>{1, 5},
                                      RowBuilder<2>{6, 1}, RowBuilder<2>{2, 2},
                                      RowBuilder<2>{R"("1")", 1}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({2, 3}, MatrixBuilder<2>{RowBuilder<2>{1, 5}, RowBuilder<2>{1, 1},
                                             RowBuilder<2>{2, 2}, RowBuilder<2>{6, 1},
                                             RowBuilder<2>{R"("1")", 1}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

// Collect with one group value and count
TEST_P(HashedCollectExecutorTest, count) {
  auto registerInfos = buildRegisterInfos(1, 3, {{1, 0}}, 2);
  auto executorInfos = buildExecutorInfos(1, 3, {{1, 0}}, 2);
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper<1, 2>()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{1}}, {{1}}, {{2}}, {{1}}, {{6}}, {{2}}, {{R"("1")"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1, 2}, {{1, 3}, {2, 2}, {6, 1}, {R"("1")", 1}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

// Collect with multiple aggregators
TEST_P(HashedCollectExecutorTest, many_aggregators) {
  auto registerInfos = buildRegisterInfos(2, 5, {{2, 0}}, RegisterPlan::MaxRegisterId,
                                          {{3, RegisterPlan::MaxRegisterId}, {4, 1}});
  auto executorInfos =
      buildExecutorInfos(2, 5, {{2, 0}}, RegisterPlan::MaxRegisterId,
                         {"LENGTH", "SUM"}, {{3, RegisterPlan::MaxRegisterId}, {4, 1}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper<2, 3>()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue(MatrixBuilder<2>{RowBuilder<2>{1, 5}, RowBuilder<2>{1, 1},
                                      RowBuilder<2>{2, 2}, RowBuilder<2>{1, 5},
                                      RowBuilder<2>{6, 1}, RowBuilder<2>{2, 2},
                                      RowBuilder<2>{3, 1}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({2, 3, 4},
                    MatrixBuilder<3>{RowBuilder<3>{1, 3, 11}, RowBuilder<3>{2, 2, 4},
                                     RowBuilder<3>{6, 1, 1}, RowBuilder<3>{3, 1, 1}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

// Collect based on equal arrays.
TEST_P(HashedCollectExecutorTest, collect_arrays) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{R"([1,1,1])"}},
                      {{1}},
                      {{R"([1,1,1,1])"}},
                      {{R"([2,1,1])"}},
                      {{R"([1,1,1])"}},
                      {{R"([2,1,1])"}},
                      {{R"([1,1,1])"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1}, {R"([1,1,1])"}, {R"([1,1,1,1])"}, {R"([2,1,1])"}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

// Collect based on equal objects.
TEST_P(HashedCollectExecutorTest, collect_objects) {
  auto registerInfos = buildRegisterInfos(1, 2, {{1, 0}});
  auto executorInfos = buildExecutorInfos(1, 2, {{1, 0}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue({{{R"({"a": 1, "b": 1})"}},
                      {{1}},
                      {{R"({"a": 1, "b": 1, "c": 1})"}},
                      {{R"({"a": 2, "b": 1})"}},
                      {{R"({"b": 1, "a": 1})"}},
                      {{R"({"b": 1, "c": 1, "a": 1})"}},
                      {{R"([1,1,1])"}}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({1}, {{1},
                          {R"([1,1,1])"},
                          {R"({"a": 1, "b": 1})"},
                          {R"({"a": 1, "b": 1, "c": 1})"},
                          {R"({"a": 2, "b": 1})"}})
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

/**
 * @section
 *
 * Test suite for aggregate functions.
 * We only do full produce here, to make sure that aggregate
 * functions work as expected.
 * The register / call implementation is tested in the other suite
 */

struct AggregateInput {
  std::string name;
  RegisterId inReg;
  MatrixBuilder<2> expectedOutput;
};

std::ostream& operator<<(std::ostream& out, AggregateInput const& agg) {
  out << agg.name;
  if (agg.inReg != RegisterPlan::MaxRegisterId) {
    out << " reg: " << agg.inReg;
  }
  return out;
}

using HashedCollectAggregateInputParam = std::tuple<HashedCollectSplitType, AggregateInput>;

class HashedCollectExecutorTestAggregate
    : public AqlExecutorTestCaseWithParam<HashedCollectAggregateInputParam> {
 protected:
  auto getSplit() -> HashedCollectSplitType {
    auto [split, unused] = GetParam();
    return split;
  }

  auto getAggregator() -> AggregateInput {
    auto [unused, info] = GetParam();
    return info;
  }

  auto buildRegisterInfos(RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                          std::vector<std::pair<RegisterId, RegisterId>> const& groupRegisters)
      -> RegisterInfos {
    RegIdSet registersToClear{};
    RegIdSetStack registersToKeep{{}};
    auto readableInputRegisters = RegIdSet{};
    auto writeableOutputRegisters = RegIdSet{};

    for (RegisterId i = 0; i < nrInputRegisters; ++i) {
      // All registers need to be invalidated!
      registersToClear.emplace(i);
    }

    for (auto const& [out, in] : groupRegisters) {
      readableInputRegisters.emplace(in);
      writeableOutputRegisters.emplace(out);
    }

    auto agg = getAggregator();
    std::vector<std::pair<RegisterId, RegisterId>> aggregateRegisters{{3, agg.inReg}};
    if (agg.inReg != RegisterPlan::MaxRegisterId) {
      readableInputRegisters.emplace(agg.inReg);
    }

    writeableOutputRegisters.emplace(3);

    return RegisterInfos{std::move(readableInputRegisters),
                         std::move(writeableOutputRegisters),
                         nrInputRegisters,
                         nrOutputRegisters,
                         registersToClear,
                         registersToKeep};
  };

  auto buildExecutorInfos(std::vector<std::pair<RegisterId, RegisterId>> groupRegisters)
      -> HashedCollectExecutorInfos {
    bool count = false;
    RegisterId collectRegister = RegisterPlan::MaxRegisterId;

    auto agg = getAggregator();
    std::vector<std::string> aggregateTypes{agg.name};
    std::vector<std::pair<RegisterId, RegisterId>> aggregateRegisters{{3, agg.inReg}};

    auto infos = HashedCollectExecutorInfos(std::move(groupRegisters), collectRegister,
                                            std::move(aggregateTypes),
                                            std::move(aggregateRegisters),
                                            &VPackOptions::Defaults, count);
    return infos;
  };
};

/**
 * Input used:
 *
 * [
 *   [1, 5]
 *   [1, 1]
 *   [2, 2]
 *   [1, 5]
 *   [6, 1]
 *   [2, 2]
 *   [3, 1]
 * ]
 * We join on the first value.
 * The second value can be used by aggregate
 */

/**
 * TODO:
 * [] Add tests for all aggregate functions
 */

auto AggregateInputs =
    ::testing::Values(AggregateInput{"LENGTH",
                                     RegisterPlan::MaxRegisterId,
                                     {{1, 3}, {2, 2}, {6, 1}, {3, 1}}},
                      AggregateInput{"SUM", 0, {{1, 3}, {2, 4}, {6, 6}, {3, 3}}},
                      AggregateInput{"SUM", 1, {{1, 11}, {2, 4}, {6, 1}, {3, 1}}});

INSTANTIATE_TEST_CASE_P(HashedCollectAggregate, HashedCollectExecutorTestAggregate,
                        ::testing::Combine(::testing::Values(splitIntoBlocks<2, 3>,
                                                             splitIntoBlocks<3, 4>,
                                                             splitStep<1>, splitStep<2>),
                                           AggregateInputs));

TEST_P(HashedCollectExecutorTestAggregate, run) {
  auto registerInfos = buildRegisterInfos(2, 4, {{2, 0}});
  auto executorInfos = buildExecutorInfos({{2, 0}});
  AqlCall call{};          // unlimited produce
  ExecutionStats stats{};  // No stats here
  makeExecutorTestHelper<2, 2>()
      .addConsumer<HashedCollectExecutor>(std::move(registerInfos), std::move(executorInfos))
      .setInputValue(MatrixBuilder<2>{RowBuilder<2>{1, 5}, RowBuilder<2>{1, 1},
                                      RowBuilder<2>{2, 2}, RowBuilder<2>{1, 5},
                                      RowBuilder<2>{6, 1}, RowBuilder<2>{2, 2},
                                      RowBuilder<2>{3, 1}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({2, 3}, getAggregator().expectedOutput)
      .allowAnyOutputOrder(true)
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      // .expectedStats(stats)
      .run();
}

}  // namespace aql
}  // namespace tests
}  // namespace arangodb
