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

#include "AqlItemBlockHelper.h"
#include "Mocks/Servers.h"
#include "TestEmptyExecutorHelper.h"
#include "TestExecutorHelper.h"
#include "TestLambdaExecutor.h"
#include "WaitingExecutionBlockMock.h"
#include "fakeit.hpp"

#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemBlockSerializationFormat.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/IdExecutor.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

using LambdaExePassThrough = TestLambdaExecutor;
using LambdaExe = TestLambdaSkipExecutor;

// This test is supposed to only test getSome return values,
// it is not supposed to test the fetch logic!

class ExecutionBlockImplTest : public ::testing::Test {
 protected:
  // ExecutionState state
  SharedAqlItemBlockPtr result;

  // Mock of the ExecutionEngine
  fakeit::Mock<ExecutionEngine> mockEngine;
  ExecutionEngine& engine;

  // Mock of the AqlItemBlockManager
  fakeit::Mock<AqlItemBlockManager> mockBlockManager;
  AqlItemBlockManager& itemBlockManager;

  // Mock of the transaction
  fakeit::Mock<transaction::Methods> mockTrx;
  transaction::Methods& trx;

  // Mock of the transaction context
  fakeit::Mock<transaction::Context> mockContext;
  transaction::Context& context;

  // Mock of the Query
  fakeit::Mock<Query> mockQuery;
  Query& query;

  ExecutionState state;
  ResourceMonitor monitor;

  // Mock of the QueryOptions
  fakeit::Mock<QueryOptions> mockQueryOptions;
  QueryOptions& lqueryOptions;
  ProfileLevel profile;

  // This is not used thus far in Base-Clase
  ExecutionNode const* node = nullptr;

  // Executor Infos
  TestExecutorHelperInfos infos;
  TestEmptyExecutorHelperInfos emptyInfos;

  SharedAqlItemBlockPtr block;

  ExecutionBlockImplTest()
      : engine(mockEngine.get()),
        itemBlockManager(mockBlockManager.get()),
        trx(mockTrx.get()),
        context(mockContext.get()),
        query(mockQuery.get()),
        lqueryOptions(mockQueryOptions.get()),
        profile(ProfileLevel(PROFILE_LEVEL_NONE)),
        node(nullptr),
        infos(0, 1, 1, {}, {0}),
        emptyInfos(0, 1, 1, {}, {0}),
        block(nullptr) {
    fakeit::When(Method(mockBlockManager, requestBlock)).AlwaysDo([&](size_t nrItems, RegisterId nrRegs) -> SharedAqlItemBlockPtr {
      return SharedAqlItemBlockPtr{new AqlItemBlock(itemBlockManager, nrItems, nrRegs)};
    });

    fakeit::When(Method(mockEngine, itemBlockManager)).AlwaysReturn(itemBlockManager);
    fakeit::When(Method(mockEngine, getQuery)).AlwaysReturn(&query);
    fakeit::When(OverloadedMethod(mockBlockManager, returnBlock, void(AqlItemBlock*&)))
        .AlwaysDo([&](AqlItemBlock*& block) -> void {
          AqlItemBlockManager::deleteBlock(block);
          block = nullptr;
        });
    fakeit::When(Method(mockBlockManager, resourceMonitor)).AlwaysReturn(&monitor);
    fakeit::When(ConstOverloadedMethod(mockQuery, queryOptions, QueryOptions const&()))
        .AlwaysDo([&]() -> QueryOptions const& { return lqueryOptions; });
    fakeit::When(OverloadedMethod(mockQuery, queryOptions, QueryOptions & ()))
        .AlwaysDo([&]() -> QueryOptions& { return lqueryOptions; });
    fakeit::When(Method(mockQuery, trx)).AlwaysReturn(&trx);

    fakeit::When(Method(mockQueryOptions, getProfileLevel)).AlwaysReturn(profile);

    fakeit::When(Method(mockTrx, transactionContextPtr)).AlwaysReturn(&context);
    fakeit::When(Method(mockContext, getVPackOptions)).AlwaysReturn(&velocypack::Options::Defaults);
  }
};

TEST_F(ExecutionBlockImplTest,
       there_is_a_block_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);

  size_t atMost = 1000;
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_NE(block, nullptr);
  ASSERT_EQ(block->size(), 1);
  ASSERT_EQ(state, ExecutionState::DONE);

  // done should stay done!
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(block, nullptr);
  ASSERT_EQ(state, ExecutionState::DONE);
}

TEST_F(ExecutionBlockImplTest,
       there_is_a_block_in_the_upstream_with_no_rows_inside_the_executor_waits_using_skipsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);

  size_t atMost = 1;
  size_t skipped = 0;

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 1);

  // done should stay done!
  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 0);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome_one_block) {
  // we are checking multiple input blocks
  // we are only fetching 1 row each (atMost = 1)
  // after a DONE is returned, it must stay done!

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 1;
  size_t total = 0;

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);

  ASSERT_EQ(total, 5);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome_multiple_blocks) {
  // as test above, BUT with a higher atMost value.

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 2;
  size_t total = 0;

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(block, nullptr);

  ASSERT_EQ(total, 5);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_skipsome) {
  // we are checking multiple input blocks
  // we are only fetching 1 row each (atMost = 1)
  // after a DONE is returned, it must stay done!

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 1;
  size_t skipped = 0;

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 0);
}

TEST_F(ExecutionBlockImplTest,
       there_is_an_invalid_empty_block_in_the_upstream_the_executor_waits_using_getsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestEmptyExecutorHelper> testee(&engine, node, std::move(emptyInfos));
  testee.addDependency(&dependency);

  size_t atMost = 1000;
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(block, nullptr);
}

/**
 * @brief Shared Test case initializer to test the execute API
 *        of the ExecutionBlockImpl implementation.
 *        This base class creates a server with a faked AQL query
 *        where we set our test node into.
 *        Also provides helper methods to create the building blocks of the query.
 */
class SharedExecutionBlockImplTest {
 protected:
  mocks::MockAqlServer server{};
  ResourceMonitor monitor{};
  std::unique_ptr<arangodb::aql::Query> fakedQuery{server.createFakeQuery()};
  std::vector<std::unique_ptr<ExecutionNode>> _execNodes;

  SharedExecutionBlockImplTest() {
    auto engine =
        std::make_unique<ExecutionEngine>(*fakedQuery, SerializationFormat::SHADOWROWS);
    fakedQuery->setEngine(engine.release());
  }

  /**
   * @brief Creates and manages a ExecutionNode.
   *        These nodes can be used to create the Executors
   *        Caller does not need to manage the memory.
   *
   * @return ExecutionNode* Pointer to a dummy ExecutionNode. Memory is managed, do not delete.
   */
  ExecutionNode* generateNodeDummy() {
    auto dummy = std::make_unique<SingletonNode>(fakedQuery->plan(), _execNodes.size());
    auto res = dummy.get();
    _execNodes.emplace_back(std::move(dummy));
    return res;
  }

  /**
   * @brief Prepare the executor infos for a LambdaExecutor with passthrough.
   *
   * @param call produceRows implementation that should be used
   * @param inputRegisters highest input register index. RegisterPlan::MaxRegisterId (default) describes there is no input. call is allowed to read any register <= inputRegisters.
   * @param outputRegisters highest output register index. RegisterPlan::MaxRegisterId (default) describes there is no output. call is allowed to write any inputRegisters < register <= outputRegisters. Invariant inputRegisters <= outputRegisters
   * @return LambdaExecutorInfos Infos to build the Executor.
   */
  LambdaExecutorInfos makeInfos(ProduceCall call,
                                RegisterId inputRegisters = RegisterPlan::MaxRegisterId,
                                RegisterId outputRegisters = RegisterPlan::MaxRegisterId) {
    if (inputRegisters != RegisterPlan::MaxRegisterId) {
      EXPECT_LE(inputRegisters, outputRegisters);
    } else if (outputRegisters != RegisterPlan::MaxRegisterId) {
      EXPECT_EQ(outputRegisters, 0);
    }

    auto readAble = make_shared_unordered_set();
    auto writeAble = make_shared_unordered_set();
    auto registersToKeep = std::unordered_set<RegisterId>{};
    if (inputRegisters != RegisterPlan::MaxRegisterId) {
      for (RegisterId i = 0; i <= inputRegisters; ++i) {
        readAble->emplace(i);
        registersToKeep.emplace(i);
      }
      for (RegisterId i = inputRegisters + 1; i <= outputRegisters; ++i) {
        writeAble->emplace(i);
      }
    } else if (outputRegisters != RegisterPlan::MaxRegisterId) {
      for (RegisterId i = 0; i <= outputRegisters; ++i) {
        writeAble->emplace(i);
      }
    }
    RegisterId regsToRead =
        (inputRegisters == RegisterPlan::MaxRegisterId) ? 0 : inputRegisters + 1;
    RegisterId regsToWrite =
        (outputRegisters == RegisterPlan::MaxRegisterId) ? 0 : outputRegisters + 1;
    return LambdaExecutorInfos(readAble, writeAble, regsToRead, regsToWrite, {},
                               registersToKeep, std::move(call));
  }

  /**
   * @brief Prepare the executor infos for a LambdaExecutor with implemented skip.
   *
   * @param call produceRows implementation that should be used
   * @param skipCall skipRowsRange implementation that should be used
   * @param inputRegisters highest input register index. RegisterPlan::MaxRegisterId (default) describes there is no input. call is allowed to read any register <= inputRegisters.
   * @param outputRegisters highest output register index. RegisterPlan::MaxRegisterId (default) describes there is no output. call is allowed to write any inputRegisters < register <= outputRegisters. Invariant inputRegisters <= outputRegisters
   * @return LambdaExecutorInfos Infos to build the Executor.
   */
  LambdaSkipExecutorInfos makeSkipInfos(ProduceCall call, SkipCall skipCall,
                                        RegisterId inputRegisters = RegisterPlan::MaxRegisterId,
                                        RegisterId outputRegisters = RegisterPlan::MaxRegisterId) {
    if (inputRegisters != RegisterPlan::MaxRegisterId) {
      EXPECT_LE(inputRegisters, outputRegisters);
    } else if (outputRegisters != RegisterPlan::MaxRegisterId) {
      EXPECT_EQ(outputRegisters, 0);
    }

    auto readAble = make_shared_unordered_set();
    auto writeAble = make_shared_unordered_set();
    auto registersToKeep = std::unordered_set<RegisterId>{};
    if (inputRegisters != RegisterPlan::MaxRegisterId) {
      for (RegisterId i = 0; i <= inputRegisters; ++i) {
        readAble->emplace(i);
        registersToKeep.emplace(i);
      }
      for (RegisterId i = inputRegisters + 1; i <= outputRegisters; ++i) {
        writeAble->emplace(i);
      }
    } else if (outputRegisters != RegisterPlan::MaxRegisterId) {
      for (RegisterId i = 0; i <= outputRegisters; ++i) {
        writeAble->emplace(i);
      }
    }
    RegisterId regsToRead =
        (inputRegisters == RegisterPlan::MaxRegisterId) ? 0 : inputRegisters + 1;
    RegisterId regsToWrite =
        (outputRegisters == RegisterPlan::MaxRegisterId) ? 0 : outputRegisters + 1;
    return LambdaSkipExecutorInfos(readAble, writeAble, regsToRead, regsToWrite,
                                   {}, registersToKeep, std::move(call),
                                   std::move(skipCall));
  }

  /**
   * @brief Generate a generic produce call with the following behaviour:
   *        1. For every input row, create a new ouput row 1:1 using copy.
   *        2. Return the input state, along with an unlimited produce call.
   *
   *        In addition we have the following assertions:
   *        1. Whenever this produce is called, it asserts that is called with the expectedCall
   *        2. This call has been called less then 10 times (emergency bailout against infinite loop)
   *        3. If there is an input row, this row is valid.
   *        4. If called with empty input, we still have exactly numRowsLeftNoInput many rows free in the output
   *        5. If called with input, we still have exactly numRowsLeftWithInput many rows free in the output
   *
   * @param nrCalls Reference! Will count how many times this function was invoked.
   * @param expectedCall The call that is expected on every invocation of this function.
   * @param numRowsLeftNoInput The number of available rows in the output, if we have empty input (cold start)
   * @param numRowsLeftWithInput The number of available rows in the output, if we have given an input
   * @return ProduceCall The call ready to hand over to the LambdaExecutorInfos
   */
  ProduceCall generateProduceCall(size_t& nrCalls, AqlCall expectedCall,
                                  size_t numRowsLeftNoInput = ExecutionBlock::DefaultBatchSize,
                                  size_t numRowsLeftWithInput = ExecutionBlock::DefaultBatchSize) {
    return [&nrCalls, numRowsLeftNoInput, numRowsLeftWithInput,
            expectedCall](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
               -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      auto const& clientCall = output.getClientCall();
      if (nrCalls > 10) {
        EXPECT_TRUE(false);
        // This is emergency bailout, we ask way to often here
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
      }
      nrCalls++;
      if (input.hasDataRow()) {
        // We expact only the empty initial row, so just consume it
        auto const [state, row] = input.nextDataRow();
        EXPECT_EQ(state, ExecutorState::DONE);
        EXPECT_TRUE(row.isInitialized());
        EXPECT_EQ(output.numRowsLeft(), numRowsLeftWithInput);
      } else {
        EXPECT_EQ(output.numRowsLeft(), numRowsLeftNoInput);
      }
      EXPECT_EQ(clientCall.getOffset(), expectedCall.getOffset());
      EXPECT_EQ(clientCall.softLimit, expectedCall.softLimit);
      EXPECT_EQ(clientCall.hardLimit, expectedCall.hardLimit);
      EXPECT_EQ(clientCall.needsFullCount(), expectedCall.needsFullCount());

      NoStats stats{};
      AqlCall call{};
      return {input.upstreamState(), stats, call};
    };
  }

  /**
   * @brief Generate a generic skip call with the following behaviour:
   *        1. For every given input: skip it, and count skip as one.
   *        2. Do never skip more then offset()
   *        3. Return the input state, the locally skipped number, a call with softLimit = offset + softLimit, hardLimit = offset + hardLimit
   *
   *        In addition we have the following assertions:
   *        1. Whenever this produce is called, it asserts that is called with the expectedCall
   *        2. This call has been called less then 10 times (emergency bailout against infinite loop)
   *        3. If there is an input row, this row is valid.
   *
   * @param nrCalls Reference! Will count how many times this function was invoked.
   * @param expectedCall The call that is expected on every invocation of this function.
   * @return SkipCall The call ready to hand over to the LambdaExecutorInfos
   */
  SkipCall generateSkipCall(size_t& nrCalls, AqlCall expectedCall) {
    return [&nrCalls,
            expectedCall](AqlItemBlockInputRange& inputRange,
                          AqlCall& clientCall) -> std::tuple<ExecutorState, size_t, AqlCall> {
      if (nrCalls > 10) {
        EXPECT_TRUE(false);
        // This is emergency bailout, we ask way to often here
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
      }
      nrCalls++;
      EXPECT_EQ(clientCall.getOffset(), expectedCall.getOffset());
      EXPECT_EQ(clientCall.softLimit, expectedCall.softLimit);
      EXPECT_EQ(clientCall.hardLimit, expectedCall.hardLimit);
      EXPECT_EQ(clientCall.needsFullCount(), expectedCall.needsFullCount());
      size_t localSkip = 0;
      while (inputRange.hasDataRow() && clientCall.getOffset() > localSkip) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        localSkip++;
      }
      clientCall.didSkip(localSkip);

      AqlCall upstreamCall = clientCall;
      upstreamCall.softLimit = clientCall.getOffset() + clientCall.softLimit;
      upstreamCall.hardLimit = clientCall.getOffset() + clientCall.hardLimit;
      upstreamCall.offset = 0;

      return {inputRange.upstreamState(), localSkip, upstreamCall};
    };
  }

  /**
   * @brief Generate a call that failes whenever it is actually called.
   *        Used to check that SKIP is not invoked
   *
   * @return SkipCall The always failing call to be used for the executor.
   */
  SkipCall generateNeverSkipCall() {
    return [](AqlItemBlockInputRange& input,
              AqlCall& call) -> std::tuple<ExecutorState, size_t, AqlCall> {
      // Should not be called here. No Skip!
      EXPECT_TRUE(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    };
  }

  /**
   * @brief Generate a call that failes whenever it is actually called.
   *        Used to check that produce is not invoked
   *
   * @return ProduceCall The always failing call to be used for the executor.
   */
  ProduceCall generateNeverProduceCall() {
    return [](AqlItemBlockInputRange& input,
              OutputAqlItemRow& output) -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      // Should not be called here. No limit, only skip!
      EXPECT_TRUE(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    };
  }
};

/**
 * @brief Test the internal statemachine of the ExecutionBlockImpl.
 *        These test-cases focus on a single executor and assert that this Executor is called
 *        correctly given an input.
 *        This is a parameterized test and tests passthrough (true) and non-passthrough variants (false)
 */
class ExecutionBlockImplExecuteSpecificTest : public SharedExecutionBlockImplTest,
                                              public testing::TestWithParam<bool> {
 protected:
  /**
   * @brief Create a Singleton ExecutionBlock. Just like the original one in the
   * query. it is already initialized and ready to use.
   *
   * @return std::unique_ptr<ExecutionBlock> The singleton ExecutionBlock.
   */
  std::unique_ptr<ExecutionBlock> createSingleton() {
    auto res = std::make_unique<ExecutionBlockImpl<IdExecutor<ConstFetcher>>>(
        fakedQuery->engine(), generateNodeDummy(), IdExecutorInfos{0, {}, {}});
    InputAqlItemRow inputRow{CreateInvalidInputRowHint{}};
    auto const [state, result] = res->initializeCursor(inputRow);
    EXPECT_EQ(state, ExecutionState::DONE);
    EXPECT_TRUE(result.ok());
    return res;
  }

  /**
   * @brief Generic test runner. Creates Lambda Executors, and returns ExecutionBlockImpl.execute(call),
   *
   * @param prod The Produce call that should be used within the Lambda Executor
   * @param skip The Skip call that should be used wiithin the Lambda Executor (only used for non-passthrough)
   * @param call The AqlCall that should be applied on the Executors.
   * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>  Response of execute(call);
   */
  auto runTest(ProduceCall& prod, SkipCall& skip, AqlCall call)
      -> std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr> {
    AqlCallStack stack{std::move(call)};
    auto singleton = createSingleton();
    if (GetParam()) {
      ExecutionBlockImpl<LambdaExePassThrough> testee{fakedQuery->engine(),
                                                      generateNodeDummy(),
                                                      makeInfos(prod)};
      testee.addDependency(singleton.get());
      return testee.execute(stack);
    } else {
      ExecutionBlockImpl<LambdaExe> testee{fakedQuery->engine(), generateNodeDummy(),
                                           makeSkipInfos(prod, skip)};
      testee.addDependency(singleton.get());
      return testee.execute(stack);
    }
  }
};

// Test a default call: no skip, no limits.
TEST_P(ExecutionBlockImplExecuteSpecificTest, test_toplevel_unlimited_call) {
  AqlCall fullCall{};
  size_t nrCalls = 0;

  // Note here: passthrough only reserves the correct amount of rows.
  // As we fetch from a singleton (1 row) we will have 0 rows (cold-start) and then exactly 1 row
  // in the executor.
  // Non passthrough does not make an estimate for this, so Batchsize is used.
  ProduceCall execImpl = GetParam() ? generateProduceCall(nrCalls, fullCall, 0, 1)
                                    : generateProduceCall(nrCalls, fullCall);
  SkipCall skipCall = generateNeverSkipCall();
  auto [state, skipped, block] = runTest(execImpl, skipCall, fullCall);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

// Test a softlimit call: no skip, given softlimit.
TEST_P(ExecutionBlockImplExecuteSpecificTest, test_toplevel_softlimit_call) {
  AqlCall fullCall{};
  fullCall.softLimit = 20;
  size_t nrCalls = 0;

  // Note here: passthrough only reserves the correct amount of rows.
  // As we fetch from a singleton (1 row) we will have 0 rows (cold-start) and then exactly 1 row
  // in the executor.
  // Non passthrough the available lines (visible to executor) are only the given soft limit.
  ProduceCall execImpl = GetParam() ? generateProduceCall(nrCalls, fullCall, 0, 1)
                                    : generateProduceCall(nrCalls, fullCall, 20, 20);
  SkipCall skipCall = generateNeverSkipCall();
  auto [state, skipped, block] = runTest(execImpl, skipCall, fullCall);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

// Test a hardlimit call: no skip, given hardlimit.
TEST_P(ExecutionBlockImplExecuteSpecificTest, test_toplevel_hardlimit_call) {
  AqlCall fullCall{};
  fullCall.hardLimit = 20;
  size_t nrCalls = 0;

  // Note here: passthrough only reserves the correct amount of rows.
  // As we fetch from a singleton (1 row) we will have 0 rows (cold-start) and then exactly 1 row
  // in the executor.
  // Non passthrough the available lines (visible to executor) are only the given soft limit.
  ProduceCall execImpl = GetParam() ? generateProduceCall(nrCalls, fullCall, 0, 1)
                                    : generateProduceCall(nrCalls, fullCall, 20, 20);
  SkipCall skipCall = generateNeverSkipCall();
  auto [state, skipped, block] = runTest(execImpl, skipCall, fullCall);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

// Test a skip call: given skip, no limits.
TEST_P(ExecutionBlockImplExecuteSpecificTest, test_toplevel_offset_call) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  size_t nrCalls = 0;

  // Note here: We skip everything, no produce should be called
  ProduceCall execImpl = generateNeverProduceCall();
  SkipCall skipCall = generateSkipCall(nrCalls, fullCall);

  auto [state, skipped, block] = runTest(execImpl, skipCall, fullCall);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  if (GetParam()) {
    // Do never call skip, pass through
    EXPECT_EQ(nrCalls, 0);
  } else {
    // Call once without input, second with input
    EXPECT_EQ(nrCalls, 2);
  }

  EXPECT_EQ(block, nullptr);
}

// Test a skip call: given skip, limit: 0 (formerly known as skipSome)
TEST_P(ExecutionBlockImplExecuteSpecificTest, test_toplevel_offset_only_call) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  // This test simulates a simple "skipSome" call on the old API.
  // It is releveant in any intermediate state.
  fullCall.softLimit = 0;
  size_t nrCalls = 0;

  // Note here: We skip everything, no produce should be called
  ProduceCall execImpl = generateNeverProduceCall();
  SkipCall skipCall = generateSkipCall(nrCalls, fullCall);

  auto [state, skipped, block] = runTest(execImpl, skipCall, fullCall);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  if (GetParam()) {
    // Do never call skip, pass through
    EXPECT_EQ(nrCalls, 0);
  } else {
    // Call once without input, second with input
    EXPECT_EQ(nrCalls, 2);
  }

  EXPECT_EQ(block, nullptr);
}

INSTANTIATE_TEST_CASE_P(ExecutionBlockImplExecuteTest,
                        ExecutionBlockImplExecuteSpecificTest, ::testing::Bool());

enum CallAsserterState { INITIAL, SKIP, GET, COUNT, DONE };

/**
 * @brief Base class for call assertions.
 * Every asserter holds an internal statemachine.
 * And is called on every invocation of the LambdaFunction.
 * According to it's internal machine, it asserts that the input Call
 * Is expected in this situation.
 *
 */
struct BaseCallAsserter {
  // Actual number of calls for this machine
  size_t call = 0;
  // Maximum allowed calls to this machine, we assert that call <= maxCall
  size_t maxCall = 0;
  // Internal state
  CallAsserterState state = CallAsserterState::DONE;
  // The expected outer call, the machine needs to extract relevant parts
  AqlCall const expected;

  // Initial state of this executor. Will return to this state on reset.
  CallAsserterState initialState = CallAsserterState::DONE;

  /**
   * @brief Construct a new Base Call Asserter object
   *
   * @param expectedCall The given outer call. As we play several rounds (e.g. one call for skip one for get) the asserter needs to decompose this call
   */
  explicit BaseCallAsserter(AqlCall const& expectedCall)
      : expected{expectedCall} {}

  virtual ~BaseCallAsserter() {}

  /**
   * @brief Reset to 0 calls and to initialState
   *
   */
  auto virtual reset() -> void {
    call = 0;
    state = initialState;
  }

  /**
   * @brief Test if we need to expect a skip phase
   *
   * @return true Yes we have skip
   * @return false No we do not have skip
   */
  auto hasSkip() const -> bool { return expected.getOffset() > 0; }
  /**
   * @brief Test if we need to expect a produce phase
   *
   * @return true
   * @return false
   */
  auto hasLimit() const -> bool { return expected.getLimit() > 0; }
  /**
   * @brief Test if we need to expect a fullcount phase
   *
   * @return true
   * @return false
   */
  auto needsFullCount() const -> bool { return expected.needsFullCount(); }

  auto gotCalled(AqlCall const& got) -> void {
    call++;
    SCOPED_TRACE("In call " + std::to_string(call) + " of " +
                 std::to_string(maxCall) + " state " + std::to_string(state));
    gotCalledWithoutTrace(got);
    EXPECT_LE(call, maxCall);
    if (call > maxCall) {
      // Security bailout to avoid infinite loops
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
  }

  virtual auto gotCalledWithoutTrace(AqlCall const& got) -> void = 0;
};

/**
 * @brief Asserter used for the skipRows implementation.
 *        Assumes that we are always called once with an empty input.
 *        And once with a given input.
 *        Will expect to be called for skip and fullCount (4 counts)
 *        Does expect to not be called if skip and/or fullCount are ommited.
 */
struct SkipCallAsserter : public BaseCallAsserter {
  bool firstCall = false;

  explicit SkipCallAsserter(AqlCall const& expectedCall)
      : BaseCallAsserter{expectedCall} {
    // Calculate number of calls
    // Ordering here is important, as it defines the start
    // state of the asserter. We first get called for skip
    // so skip needs to be last here
    if (needsFullCount()) {
      maxCall += 2;
      initialState = CallAsserterState::COUNT;
    }
    if (hasSkip()) {
      maxCall += 2;
      initialState = CallAsserterState::SKIP;
    }
    // It is possible that we actually have 0 calls.
    // if there is neither skip nor limit
    state = initialState;
  }

  auto reset() -> void override {
    BaseCallAsserter::reset();
    firstCall = false;
  }

  auto gotCalledWithoutTrace(AqlCall const& got) -> void override {
    switch (state) {
      case CallAsserterState::SKIP: {
        EXPECT_EQ(got.getOffset(), expected.getOffset());
        firstCall = !firstCall;
        if (!firstCall) {
          if (needsFullCount()) {
            state = CallAsserterState::COUNT;
          } else {
            state = CallAsserterState::DONE;
          }
        }
        break;
      }
      case CallAsserterState::COUNT: {
        EXPECT_EQ(got.getLimit(), 0);
        EXPECT_EQ(got.getOffset(), 0);
        EXPECT_TRUE(got.needsFullCount());
        firstCall = !firstCall;
        if (!firstCall) {
          state = CallAsserterState::DONE;
        }
        break;
      }
      case CallAsserterState::INITIAL:
      case CallAsserterState::GET:
      case CallAsserterState::DONE: {
        // This should not be reached
        EXPECT_FALSE(true);
        break;
      }
    }
  }
};

/**
 * @brief Asserter used for the produce method.
 *        Asserts to be called twice if data is requested. (limit > 0)
 *        Once with, once without data.
 */
struct CallAsserter : public BaseCallAsserter {
  explicit CallAsserter(AqlCall const& expectedCall)
      : BaseCallAsserter{expectedCall} {
    // Calculate number of calls
    if (hasLimit()) {
      maxCall += 2;
      initialState = CallAsserterState::INITIAL;
    }
    // It is possible that we actually have 0 calls.
    // if there is neither skip nor limit
    state = initialState;
  }

  auto gotCalledWithoutTrace(AqlCall const& got) -> void override {
    EXPECT_EQ(got.getOffset(), 0);
    switch (state) {
      case CallAsserterState::INITIAL: {
        EXPECT_EQ(got.getLimit(), expected.getLimit());
        state = CallAsserterState::GET;
        break;
      }
      case CallAsserterState::GET: {
        EXPECT_EQ(got.getLimit(), expected.getLimit());
        state = CallAsserterState::DONE;
        break;
      }
      case CallAsserterState::SKIP:
      case CallAsserterState::COUNT:
      case CallAsserterState::DONE: {
        // This should not be reached
        EXPECT_FALSE(true);
        break;
      }
    }
  }
};

/**
 * @brief Asserter used "above" an executor that implements
 *        skip and produce, and transforms everything to produce.
 *        Expects to be called twice for each sitation (with and without input).
 *        Expect up to three situations: SKIP, GET and FULLCOUNT.
 */
struct GetOnlyCallAsserter : public BaseCallAsserter {
  bool firstCall = false;

  explicit GetOnlyCallAsserter(AqlCall const& expectedCall)
      : BaseCallAsserter{expectedCall} {
    // Calculate number of calls
    // Ordering here is important, as it defines the start
    // state of the asserter. We first get called for skip
    // so skip needs to be last here
    if (needsFullCount()) {
      maxCall += 2;
      initialState = CallAsserterState::COUNT;
    }
    if (hasLimit()) {
      maxCall += 2;
      initialState = CallAsserterState::GET;
    }
    if (hasSkip()) {
      maxCall += 2;
      initialState = CallAsserterState::SKIP;
    }
    state = initialState;
    // Make sure setup worked
    EXPECT_GT(maxCall, 0);
    EXPECT_NE(state, CallAsserterState::DONE);
  }

  auto reset() -> void override {
    BaseCallAsserter::reset();
    firstCall = false;
  }

  auto gotCalledWithoutTrace(AqlCall const& got) -> void override {
    EXPECT_EQ(got.getOffset(), 0);
    EXPECT_FALSE(got.needsFullCount());

    switch (state) {
      case CallAsserterState::SKIP: {
        EXPECT_EQ(got.getLimit(), expected.getOffset());
        firstCall = !firstCall;
        if (!firstCall) {
          // We only switch to next state every second call.
          // The first call is "empty" and only forwards to upwards
          if (hasLimit()) {
            state = CallAsserterState::GET;
          } else if (needsFullCount()) {
            state = CallAsserterState::COUNT;
          } else {
            state = CallAsserterState::DONE;
          }
        }
        break;
      }
      case CallAsserterState::GET: {
        EXPECT_EQ(got.getLimit(), expected.getLimit());
        firstCall = !firstCall;
        if (!firstCall) {
          // We only switch to next state every second call.
          // The first call is "empty" and only forwards to upwards
          if (needsFullCount()) {
            state = CallAsserterState::COUNT;
          } else {
            state = CallAsserterState::DONE;
          }
        }
        break;
      }

      case CallAsserterState::COUNT: {
        // We do not test 0,0,false
        EXPECT_TRUE(needsFullCount());
        EXPECT_EQ(got.softLimit, AqlCall::Infinity{});
        EXPECT_EQ(got.hardLimit, AqlCall::Infinity{});
        firstCall = !firstCall;
        if (!firstCall) {
          // We only switch to next state every second call.
          // The first call is "empty" and only forwards to upwards
          state = CallAsserterState::DONE;
        }
        break;
      }
      case CallAsserterState::INITIAL:
      case CallAsserterState::DONE: {
        // This should not be reached
        EXPECT_FALSE(true);
        break;
      }
    }
  }
};

/**
 * @brief None asserter, does not assert any thing within a call.
 * Only asserts that we are not called more than maxCalls times.
 *
 */
struct NoneAsserter : public BaseCallAsserter {
  explicit NoneAsserter(AqlCall const& expectedCall, size_t maxCalls)
      : BaseCallAsserter{expectedCall} {
    maxCall = maxCalls;
  }

  auto gotCalledWithoutTrace(AqlCall const& got) -> void override {}
};

/**
 * @brief Integration tests.
 *        These test tests a chain of Executors.
 *        It focuses on the part that all executors
 *        get injected the correct calls in each iteration
 *        of the Execute state machine.
 *        Also asserts that "UPSTREAM" is called with the correct
 *        forwarded call.
 *        This is a parameterized testsuite that uses a set of pseudo-random AqlCalls of different formats.
 *        The second parameter is a boolean to flag if we use WAITING on singleton.
 */
class ExecutionBlockImplExecuteIntegrationTest
    : public SharedExecutionBlockImplTest,
      public testing::TestWithParam<std::tuple<AqlCall, bool>> {
 protected:
  /**
   * @brief Get the Call object
   *
   * @return AqlCall used as test parameter
   */
  AqlCall getCall() const {
    auto const [call, waits] = GetParam();
    return call;
  }

  /**
   * @brief Get the combination if we are waiting or not.
   *
   * @return true We need waiting
   * @return false We do not.
   */
  bool doesWaiting() const {
    auto const [call, waits] = GetParam();
    return waits;
  }

  /**
   * @brief Create a Singleton ExecutionBlock. Just like the original one in the
   * query. it is already initialized and ready to use.
   *
   * @return std::unique_ptr<ExecutionBlock> The singleton ExecutionBlock.
   */
  std::unique_ptr<ExecutionBlock> createSingleton() {
    std::deque<SharedAqlItemBlockPtr> blockDeque;
    SharedAqlItemBlockPtr block =
        buildBlock<0>(fakedQuery->engine()->itemBlockManager(), {{}});
    blockDeque.push_back(std::move(block));
    return std::make_unique<WaitingExecutionBlockMock>(
        fakedQuery->engine(), generateNodeDummy(), std::move(blockDeque),
        doesWaiting() ? WaitingExecutionBlockMock::WaitingBehaviour::ALLWAYS
                      : WaitingExecutionBlockMock::WaitingBehaviour::NEVER);
  }

  /**
   * @brief Create a Producing ExecutionBlock
   *        For every input row this block will write the array given in data
   *        into the output once.
   *        Each entry in the array goes into one line and is writen into outReg.
   *
   * @param dependency The dependecy of this block (produces input)
   * @param data The data to be written, needs to be an array.
   * @param outReg The register to be written to
   * @return std::unique_ptr<ExecutionBlock> ready to use ProducerBlock.
   */
  std::unique_ptr<ExecutionBlock> produceBlock(ExecutionBlock* dependency,
                                               std::shared_ptr<VPackBuilder> data,
                                               RegisterId outReg) {
    TRI_ASSERT(dependency != nullptr);
    TRI_ASSERT(data != nullptr);
    TRI_ASSERT(data->slice().isArray());
    // We make this a shared ptr just to make sure someone retains the data.
    auto iterator = std::make_shared<VPackArrayIterator>(data->slice());
    auto writeData = [data, outReg, iterator](AqlItemBlockInputRange& inputRange,
                                              OutputAqlItemRow& output)
        -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      while (inputRange.hasDataRow() && !output.isFull()) {
        auto const& [state, input] = inputRange.peekDataRow();
        EXPECT_TRUE(input.isInitialized());
        while (!output.isFull() && iterator->valid()) {
          output.cloneValueInto(outReg, input, AqlValue{iterator->value()});
          output.advanceRow();
          iterator->next();
        }
        if (!iterator->valid()) {
          // Consume input
          auto const& [state, input] = inputRange.nextDataRow();
          EXPECT_TRUE(input.isInitialized());
          iterator->reset();
        }
      }
      // We always use a default unlimited call here, we only have Singleton above.
      AqlCall call{};
      return {inputRange.upstreamState(), NoStats{}, call};
    };

    auto skipData =
        [data, iterator](AqlItemBlockInputRange& inputRange,
                         AqlCall& clientCall) -> std::tuple<ExecutorState, size_t, AqlCall> {
      size_t skipped = 0;
      while (inputRange.hasDataRow() &&
             (clientCall.getOffset() > 0 ||
              (clientCall.getLimit() == 0 && clientCall.needsFullCount()))) {
        auto const& [state, input] = inputRange.peekDataRow();
        EXPECT_TRUE(input.isInitialized());
        while ((clientCall.getOffset() > 0 ||
                (clientCall.getLimit() == 0 && clientCall.needsFullCount())) &&
               iterator->valid()) {
          clientCall.didSkip(1);
          skipped++;
          iterator->next();
        }
        if (!iterator->valid()) {
          // Consume input
          auto const& [state, input] = inputRange.nextDataRow();
          EXPECT_TRUE(input.isInitialized());
          iterator->reset();
        }
      }
      AqlCall call{};
      call.offset = 0;
      if (clientCall.getOffset() > 0) {
        call.softLimit = clientCall.getOffset();
      }  // else softLimit == unlimited
      call.fullCount = false;
      return {inputRange.upstreamState(), skipped, call};
    };
    auto infos = outReg == 0
                     ? makeSkipInfos(std::move(writeData), skipData,
                                     RegisterPlan::MaxRegisterId, outReg)
                     : makeSkipInfos(std::move(writeData), skipData, outReg - 1, outReg);
    auto producer =
        std::make_unique<ExecutionBlockImpl<LambdaExe>>(fakedQuery->engine(),
                                                        generateNodeDummy(),
                                                        std::move(infos));
    producer->addDependency(dependency);
    return producer;
  }

  /**
   * @brief Create a simple row forwarding Block.
   *        It simply takes one input row and copies it into the output
   *
   * @param asserter A call asserter, that will invoke getCalled on every call
   * @param dependency The dependecy of this block (produces input)
   * @param maxReg The number of registers in input and output. (required for forwarding of data)
   * @return std::unique_ptr<ExecutionBlock> ready to use ForwardingBlock.
   */
  std::unique_ptr<ExecutionBlock> forwardBlock(BaseCallAsserter& asserter,
                                               ExecutionBlock* dependency,
                                               RegisterId maxReg) {
    TRI_ASSERT(dependency != nullptr);
    auto forwardData = [&asserter](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
        -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      asserter.gotCalled(output.getClientCall());
      while (inputRange.hasDataRow() && !output.isFull()) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        output.copyRow(input);
        output.advanceRow();
      }
      return {inputRange.upstreamState(), NoStats{}, output.getClientCall()};
    };
    auto producer = std::make_unique<ExecutionBlockImpl<LambdaExePassThrough>>(
        fakedQuery->engine(), generateNodeDummy(),
        makeInfos(std::move(forwardData), maxReg, maxReg));
    producer->addDependency(dependency);
    return producer;
  }

  /**
   * @brief Create a simple row forwarding Block.
   *        It simply takes one input row and copies it into the output.
   *        Implements Skip
   *
   * @param produceAsserter A call asserter, that will invoke getCalled on every produce call
   * @param skipAsserter A call asserter, that will invoke getCalled on every skip call
   * @param dependency The dependecy of this block (produces input)
   * @param maxReg The number of registers in input and output. (required for forwarding of data)
   * @return std::unique_ptr<ExecutionBlock> ready to use ForwardingBlock.
   */
  std::unique_ptr<ExecutionBlock> forwardBlock(BaseCallAsserter& produceAsserter,
                                               BaseCallAsserter& skipAsserter,
                                               ExecutionBlock* dependency,
                                               RegisterId maxReg) {
    TRI_ASSERT(dependency != nullptr);
    auto forwardData = [&produceAsserter](AqlItemBlockInputRange& inputRange,
                                          OutputAqlItemRow& output)
        -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      produceAsserter.gotCalled(output.getClientCall());
      while (inputRange.hasDataRow() && !output.isFull()) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        output.copyRow(input);
        output.advanceRow();
      }
      return {inputRange.upstreamState(), NoStats{}, output.getClientCall()};
    };

    auto skipData = [&skipAsserter](AqlItemBlockInputRange& inputRange,
                                    AqlCall& call) -> std::tuple<ExecutorState, size_t, AqlCall> {
      skipAsserter.gotCalled(call);

      size_t skipped = 0;
      while (inputRange.hasDataRow() && call.shouldSkip()) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        skipped++;
        call.didSkip(1);
      }
      // Do forward a softLimit call only.
      // Do not oeverfetch here.
      AqlCall request;
      if (call.getOffset() > 0) {
        request.softLimit = call.getOffset();
      }  // else fullCount case, simple get UNLIMITED from above

      return {inputRange.upstreamState(), skipped, request};
    };
    auto producer = std::make_unique<ExecutionBlockImpl<LambdaExe>>(
        fakedQuery->engine(), generateNodeDummy(),
        makeSkipInfos(std::move(forwardData), std::move(skipData), maxReg, maxReg));
    producer->addDependency(dependency);
    return producer;
  }

  void ValidateSkipMatches(AqlCall const& call, size_t dataLength, size_t actual) const {
    size_t expected = 0;
    // Skip Offset, but not more then available
    expected += std::min(call.getOffset(), dataLength);
    if (call.needsFullCount()) {
      // We can only fullCount on hardlimit. If this fails check test code!
      EXPECT_TRUE(call.hasHardLimit());
      // We consume either hardLimit + offset, or all data.
      size_t consumed = std::min(call.getLimit() + call.getOffset(), dataLength);
      // consumed >= dataLength, if it is smaller we have a remainder for fullCount.
      expected += dataLength - consumed;
    }
    EXPECT_EQ(expected, actual);
  }

  /**
   * @brief Helper method to validate the result
   *        It will take into account the call used as Parameter
   *        and slice the expectated outcome to it.
   *
   * It asserts the following:
   *   1. skipped == offset() + (data.length - hardLimit [fullcount])
   *   2. result.length = (hardLimit||data.length) - offset.
   *   3. result register entry matches the entry at the correct position in data.
   *
   * @param data The data to be expected, if we would just get it in full
   * @param skipped The number of rows the executor reported as skipped
   * @param result The resulting data output
   * @param testReg The register to evaluate
   * @param numShadowRows Number of preceeding shadowRows in result.
   */
  void ValidateResult(std::shared_ptr<VPackBuilder> data, size_t skipped,
                      SharedAqlItemBlockPtr result, RegisterId testReg,
                      size_t numShadowRows = 0) {
    auto const& call = getCall();

    TRI_ASSERT(data != nullptr);
    TRI_ASSERT(data->slice().isArray());

    VPackSlice expected = data->slice();
    ValidateSkipMatches(call, static_cast<size_t>(expected.length()), skipped);

    VPackArrayIterator expectedIt{expected};
    // Skip Part
    size_t offset =
        (std::min)(call.getOffset(), static_cast<size_t>(expected.length()));

    for (size_t i = 0; i < offset; ++i) {
      // The first have been skipped
      expectedIt++;
    }
    size_t limit =
        (std::min)(call.getLimit(), static_cast<size_t>(expected.length()) - offset);
    if (result != nullptr && result->size() > numShadowRows) {
      // GetSome part
      EXPECT_EQ(limit, result->size() - numShadowRows);
      for (size_t i = 0; i < limit; ++i) {
        // The next have to match
        auto got = result->getValueReference(i, testReg).slice();
        EXPECT_TRUE(basics::VelocyPackHelper::equal(got, *expectedIt, false))
            << "Expected: " << expectedIt.value().toJson() << " got: " << got.toJson()
            << " in row " << i << " and register " << testReg;
        expectedIt++;
      }
    } else {
      EXPECT_EQ(limit, 0);
    }
  }
};

// Test a simple produce block. that has is supposed to write 1000 rows.
TEST_P(ExecutionBlockImplExecuteIntegrationTest, test_produce_only) {
  auto singleton = createSingleton();

  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 1000; ++i) {
    builder->add(VPackValue(i));
  }
  builder->close();
  RegisterId outReg = 0;
  auto producer = produceBlock(singleton.get(), builder, outReg);

  auto const& call = getCall();
  AqlCallStack stack{call};
  if (doesWaiting()) {
    auto const [state, skipped, block] = producer->execute(stack);
    EXPECT_EQ(state, ExecutionState::WAITING);
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(block, nullptr);
  }
  auto const [state, skipped, block] = producer->execute(stack);
  if (std::holds_alternative<size_t>(call.softLimit) && !call.hasHardLimit()) {
    EXPECT_EQ(state, ExecutionState::HASMORE);
  } else {
    EXPECT_EQ(state, ExecutionState::DONE);
  }

  ValidateResult(builder, skipped, block, outReg);
}

// Test two consecutive produce blocks.
// The first writes 10 lines
// The second another 100 per input (1000 in total)
TEST_P(ExecutionBlockImplExecuteIntegrationTest, test_produce_using_two) {
  auto singleton = createSingleton();

  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 10; ++i) {
    builder->add(VPackValue(i));
  }
  builder->close();
  RegisterId outRegFirst = 0;
  RegisterId outRegSecond = 1;
  auto producerFirst = produceBlock(singleton.get(), builder, outRegFirst);
  auto producer = produceBlock(producerFirst.get(), builder, outRegSecond);
  auto const& call = getCall();
  AqlCallStack stack{call};
  if (doesWaiting()) {
    auto const [state, skipped, block] = producer->execute(stack);
    EXPECT_EQ(state, ExecutionState::WAITING);
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(block, nullptr);
  }
  auto const [state, skipped, block] = producer->execute(stack);
  if (call.getLimit() < 100) {
    if (call.hasHardLimit()) {
      // On hard limit we need to stop
      EXPECT_EQ(state, ExecutionState::DONE);
    } else {
      // On soft limit we need to be able to produce more
      EXPECT_EQ(state, ExecutionState::HASMORE);
    }
  } else {
    EXPECT_FALSE(call.hasHardLimit());
    EXPECT_EQ(state, ExecutionState::DONE);
  }

  auto firstRegBuilder = std::make_shared<VPackBuilder>();
  auto secondRegBuilder = std::make_shared<VPackBuilder>();
  firstRegBuilder->openArray();
  secondRegBuilder->openArray();
  for (size_t i = 0; i < 10; ++i) {
    // i => 0 -> 9
    for (size_t j = 0; j < 10; ++j) {
      // j => 0 -> 9
      firstRegBuilder->add(VPackValue(i));
      secondRegBuilder->add(VPackValue(j));
    }
  }
  secondRegBuilder->close();
  firstRegBuilder->close();
  ValidateResult(firstRegBuilder, skipped, block, outRegFirst);
  ValidateResult(secondRegBuilder, skipped, block, outRegSecond);
}

// Explicitly test call forwarding, on exectors.
// We use two pass-through producers, that simply copy over input and assert an calls.
// On top of them we have a 1000 line producer.
// We expect the result to be identical to the 1000 line producer only.
TEST_P(ExecutionBlockImplExecuteIntegrationTest, test_call_forwarding_passthrough) {
  auto singleton = createSingleton();

  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 1000; ++i) {
    builder->add(VPackValue(i));
  }
  builder->close();
  RegisterId outReg = 0;
  auto producer = produceBlock(singleton.get(), builder, outReg);

  CallAsserter upperState{getCall()};
  auto upper = forwardBlock(upperState, producer.get(), outReg);
  CallAsserter lowerState{getCall()};
  auto lower = forwardBlock(lowerState, upper.get(), outReg);

  auto const& call = getCall();
  AqlCallStack stack{call};
  if (doesWaiting()) {
    auto const [state, skipped, block] = lower->execute(stack);
    EXPECT_EQ(state, ExecutionState::WAITING);
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(block, nullptr);
    // Reset call counters
    upperState.reset();
    lowerState.reset();
  }
  auto const [state, skipped, block] = lower->execute(stack);
  if (std::holds_alternative<size_t>(call.softLimit) && !call.hasHardLimit()) {
    EXPECT_EQ(state, ExecutionState::HASMORE);
  } else {
    EXPECT_EQ(state, ExecutionState::DONE);
  }
  ValidateResult(builder, skipped, block, outReg);
}

// Explicitly test call forwarding, on exectors.
// We use one pass-through producer, that simply copy over input and assert an calls.
// And we have one non-passthrough below it, that requests all data from upstream, and internally
// does skipping.
// On top of them we have a 1000 line producer.
// We expect the result to be identical to the 1000 line producer only.
TEST_P(ExecutionBlockImplExecuteIntegrationTest, test_call_forwarding_implement_skip) {
  auto singleton = createSingleton();

  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 1000; ++i) {
    builder->add(VPackValue(i));
  }
  builder->close();
  RegisterId outReg = 0;
  auto producer = produceBlock(singleton.get(), builder, outReg);

  GetOnlyCallAsserter upperState{getCall()};
  auto upper = forwardBlock(upperState, producer.get(), outReg);

  CallAsserter lowerState{getCall()};
  SkipCallAsserter skipState{getCall()};

  auto forwardCall = [&](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
    lowerState.gotCalled(output.getClientCall());
    while (inputRange.hasDataRow() && !output.isFull()) {
      auto const& [state, input] = inputRange.nextDataRow();
      EXPECT_TRUE(input.isInitialized());
      output.copyRow(input);
      output.advanceRow();
    }
    auto getClient = output.getClientCall();
    AqlCall request{};
    request.softLimit = (std::min)(getClient.softLimit, getClient.hardLimit);
    return {inputRange.upstreamState(), NoStats{}, request};
  };
  auto forwardSkipCall = [&](AqlItemBlockInputRange& inputRange,
                             AqlCall& call) -> std::tuple<ExecutorState, size_t, AqlCall> {
    skipState.gotCalled(call);
    size_t skipped = 0;
    while (inputRange.hasDataRow() && call.shouldSkip()) {
      auto const& [state, input] = inputRange.nextDataRow();
      EXPECT_TRUE(input.isInitialized());
      skipped++;
      call.didSkip(1);
    }
    // Do forward a softLimit call only.
    // Do not oeverfetch here.
    AqlCall request;
    if (call.getOffset() > 0) {
      request.softLimit = call.getOffset();
    }  // else fullCount case, simple get UNLIMITED from above

    return {inputRange.upstreamState(), skipped, request};
  };

  auto lower = std::make_unique<ExecutionBlockImpl<TestLambdaSkipExecutor>>(
      fakedQuery->engine(), generateNodeDummy(),
      makeSkipInfos(std::move(forwardCall), std::move(forwardSkipCall), outReg, outReg));
  lower->addDependency(upper.get());

  auto const& call = getCall();
  AqlCallStack stack{call};
  if (doesWaiting()) {
    auto const [state, skipped, block] = lower->execute(stack);
    EXPECT_EQ(state, ExecutionState::WAITING);
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(block, nullptr);
    upperState.reset();
    lowerState.reset();
    skipState.reset();
  }
  auto const [state, skipped, block] = lower->execute(stack);
  if (std::holds_alternative<size_t>(call.softLimit) && !call.hasHardLimit()) {
    EXPECT_EQ(state, ExecutionState::HASMORE);
  } else {
    EXPECT_EQ(state, ExecutionState::DONE);
  }
  ValidateResult(builder, skipped, block, outReg);
}

TEST_P(ExecutionBlockImplExecuteIntegrationTest, DISABLED_test_multiple_upstream_calls) {
  // The WAITING block mock can only stop returning after a full block.
  // As the used calls have "random" sizes, we simply create 1 line blocks only.
  // This is less then optimal, but we will have an easily predictable result, with a complex internal structure
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 1000; ++i) {
    SharedAqlItemBlockPtr block =
        buildBlock<1>(fakedQuery->engine()->itemBlockManager(), {{i}});
    blockDeque.push_back(std::move(block));
    builder->add(VPackValue(i));
  }
  builder->close();

  auto producer = std::make_unique<WaitingExecutionBlockMock>(
      fakedQuery->engine(), generateNodeDummy(), std::move(blockDeque),
      doesWaiting() ? WaitingExecutionBlockMock::WaitingBehaviour::ALLWAYS
                    : WaitingExecutionBlockMock::WaitingBehaviour::NEVER);

  NoneAsserter produceAsserter{getCall(), ExecutionBlock::DefaultBatchSize * 3};
  NoneAsserter skipAsserter{getCall(), ExecutionBlock::DefaultBatchSize * 3};
  RegisterId outReg = 0;
  auto testee = forwardBlock(produceAsserter, skipAsserter, producer.get(), outReg);
  auto const& call = getCall();
  AqlCallStack stack{call};
  auto [state, skipped, block] = testee->execute(stack);
  size_t killSwitch = 0;
  while (state == ExecutionState::WAITING) {
    EXPECT_TRUE(doesWaiting());
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(block, nullptr);
    std::tie(state, skipped, block) = testee->execute(stack);
    // Kill switch to avoid endless loop in case of error.
    // We should get this through with much fewer than two times Batchsize calls.
    killSwitch++;
    if (killSwitch >= ExecutionBlock::DefaultBatchSize * 2) {
      ASSERT_TRUE(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
  }
  if (std::holds_alternative<size_t>(call.softLimit) && !call.hasHardLimit()) {
    EXPECT_EQ(state, ExecutionState::HASMORE);
  } else {
    EXPECT_EQ(state, ExecutionState::DONE);
  }

  ValidateResult(builder, skipped, block, outReg);
}

TEST_P(ExecutionBlockImplExecuteIntegrationTest,
       DISABLED_test_multiple_upstream_calls_passthrough) {
  // The WAITING block mock can only stop returning after a full block.
  // As the used calls have "random" sizes, we simply create 1 line blocks only.
  // This is less then optimal, but we will have an easily predictable result, with a complex internal structure
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 1000; ++i) {
    SharedAqlItemBlockPtr block =
        buildBlock<1>(fakedQuery->engine()->itemBlockManager(), {{i}});
    blockDeque.push_back(std::move(block));
    builder->add(VPackValue(i));
  }
  builder->close();

  auto producer = std::make_unique<WaitingExecutionBlockMock>(
      fakedQuery->engine(), generateNodeDummy(), std::move(blockDeque),
      doesWaiting() ? WaitingExecutionBlockMock::WaitingBehaviour::ALLWAYS
                    : WaitingExecutionBlockMock::WaitingBehaviour::NEVER);

  NoneAsserter produceAsserter{getCall(), ExecutionBlock::DefaultBatchSize * 3};
  RegisterId outReg = 0;
  auto testee = forwardBlock(produceAsserter, producer.get(), outReg);
  auto const& call = getCall();
  auto limit = call.getLimit();
  size_t offset = call.getOffset();
  bool fullCount = call.needsFullCount();
  AqlCallStack stack{call};

  if (limit == 0) {
    // we can bypass everything and get away with a single call
    auto [state, skipped, block] = testee->execute(stack);
    if (doesWaiting()) {
      size_t waited = 0;
      while (state == ExecutionState::WAITING && waited < 1010 /* avoid endless waiting*/) {
        EXPECT_EQ(state, ExecutionState::WAITING);
        EXPECT_EQ(skipped, 0);
        EXPECT_EQ(block, nullptr);
        waited++;
        std::tie(state, skipped, block) = testee->execute(stack);
      }
      EXPECT_LT(1000, waited);
    }
    EXPECT_EQ(block, nullptr);
    if (fullCount) {
      // We skipped everything
      EXPECT_EQ(skipped, 1000);
      EXPECT_EQ(state, ExecutionState::DONE);
    } else {
      EXPECT_EQ(skipped, offset);
      EXPECT_EQ(state, ExecutionState::HASMORE);
    }
  } else {
    VPackArrayIterator it{builder->slice()};
    // Skip over offset
    for (size_t i = 0; i < offset; ++i) {
      ++it;
    }
    for (size_t i = 0; i < limit && it.valid(); ++i) {
      auto [state, skipped, block] = testee->execute(stack);
      if (doesWaiting()) {
        size_t waited = 0;
        while (state == ExecutionState::WAITING &&
               waited < offset + 10 /* avoid endless waiting*/) {
          EXPECT_EQ(state, ExecutionState::WAITING);
          EXPECT_EQ(skipped, 0);
          EXPECT_EQ(block, nullptr);
          waited++;
          std::tie(state, skipped, block) = testee->execute(stack);
        }
        if (offset > 0 && i == 0) {
          // We wait some time before the first row is produced
          EXPECT_LT(1000, waited);
        } else {
          // We wait once, then we we get a line.
          EXPECT_EQ(1, waited);
        }
      }
      ASSERT_NE(block, nullptr);
      auto got = block->getValueReference(0, outReg).slice();
      EXPECT_TRUE(basics::VelocyPackHelper::equal(got, *it, false))
          << "Expected: " << it.value().toJson() << " got: " << got.toJson()
          << " in row " << i << " and register " << outReg;
      if (i == 0) {
        // The first data row includes skip
        EXPECT_EQ(skipped, offset);
      } else if (i + 1 == limit && call.hasHardLimit() && fullCount) {
        // The last data row includes the fullcount
        EXPECT_EQ(skipped, 1000 - limit - offset);
      } else {
        // No more skipping on later data rows
        EXPECT_EQ(skipped, 0);
      }
      if (i + 1 == limit && call.hasHardLimit()) {
        // The last hardLimit row contains DONE
        EXPECT_EQ(state, ExecutionState::DONE);
      } else {
        EXPECT_EQ(state, ExecutionState::HASMORE);
      }
      it++;
    }
  }
}

/* TODO
 * [] Test in between waiting variant
 * [] Test shadowRows
 *   [] ShadowRows at end of block forwarding
 *   [] ShadowRow BlockEnd ShadowRow higher depth
 *   [] ShadowRow BlockEnd ShadowRow equal depth
 */

/*
// We simulate the situation within a subquery.
// Everysubquery result fits into a single output block (including shadow rows).
// So we assert that executors only return results of the subquery they are part of in one go.
TEST_P(ExecutionBlockImplExecuteIntegrationTest, test_simulate_subquery_execution) {
  auto singleton = createSingleton();
  auto subqueryBuilder = std::make_shared<VPackBuilder>();
  subqueryBuilder->openArray();
  for (size_t i = 0; i < 10; ++i) {
    subqueryBuilder->add(VPackValue(i));
  }
  subqueryBuilder->close();
  RegisterId subqueryReg = 0;
  // We start with 10 times data
  auto subqueryStarter = produceBlock(singleton.get(), subqueryBuilder, subqueryReg);

  // We start 10 subqueries => (input, shadowRow)
  auto shadowRowWriter = produceSubqueriesBlock(subqueryStarter.get());

  auto builder = std::make_shared<VPackBuilder>();
  builder->openArray();
  for (size_t i = 0; i < 100; ++i) {
    builder->add(VPackValue(i));
  }
  builder->close();
  RegisterId outReg = 1;
  // In each we write 100 dataRows => (10 input, shadowRow)
  // Note 100 is important here as our tests have offset + hardLimit > 100
  auto call = getCall();
  ASSERT_GT(call.getOffset() + call.getLimit(), 100);
  auto producer = produceBlock(subqueryStarter.get(), builder, outReg);

  // To assert the calls, include a passthrough forwarder.
  CallAsserter asserter{getCall()};
  auto forwarder = forwardBlock(asserter, producer.get(), outReg);

  // Now analyze
  // We expect 9 identical runs of execute.
  // Each ending with HASMORE on a shadowRow
  // The tenth run ends with DONE on a shadowRow
  for (size_t i = 0; i < 10; ++i) {
    auto const& call = getCall();
    AqlCallStack stack{call};
    auto const [state, skipped, block] = forwarder->execute(stack);
    if (i < 9) {
      EXPECT_EQ(state, ExecutionState::HASMORE);
    } else {
      EXPECT_EQ(state, ExecutionState::DONE);
    }
    ValidateResult(builder, skipped, block, outReg, 1);
  }
}
*/

// The numbers here are random, but all of them are below 1000 which is the default batch size
static constexpr auto defaultCall = []() -> const AqlCall { return AqlCall{}; };

static constexpr auto skipCall = []() -> const AqlCall {
  AqlCall res{};
  res.offset = 15;
  return res;
};

static constexpr auto softLimit = []() -> const AqlCall {
  AqlCall res{};
  res.softLimit = 35;
  return res;
};

static constexpr auto hardLimit = []() -> const AqlCall {
  AqlCall res{};
  res.hardLimit = 76;
  return res;
};

static constexpr auto fullCount = []() -> const AqlCall {
  AqlCall res{};
  res.hardLimit = 17;
  res.fullCount = true;
  return res;
};

static constexpr auto skipAndSoftLimit = []() -> const AqlCall {
  AqlCall res{};
  res.offset = 16;
  res.softLimit = 64;
  return res;
};

static constexpr auto skipAndHardLimit = []() -> const AqlCall {
  AqlCall res{};
  res.offset = 32;
  res.hardLimit = 51;
  return res;
};
static constexpr auto skipAndHardLimitAndFullCount = []() -> const AqlCall {
  AqlCall res{};
  res.offset = 8;
  res.hardLimit = 57;
  res.fullCount = true;
  return res;
};
static constexpr auto onlyFullCount = []() -> const AqlCall {
  AqlCall res{};
  res.hardLimit = 0;
  res.fullCount = true;
  return res;
};
static constexpr auto onlySkipAndCount = []() -> const AqlCall {
  AqlCall res{};
  res.offset = 16;
  res.hardLimit = 0;
  res.fullCount = true;
  return res;
};

INSTANTIATE_TEST_CASE_P(
    ExecutionBlockExecuteIntegration, ExecutionBlockImplExecuteIntegrationTest,
    ::testing::Combine(::testing::Values(defaultCall(), skipCall(), softLimit(),
                                         hardLimit(), fullCount(), skipAndSoftLimit(),
                                         skipAndHardLimit(), skipAndHardLimitAndFullCount(),
                                         onlyFullCount(), onlySkipAndCount()),
                       ::testing::Bool()));

}  // namespace aql
}  // namespace tests
}  // namespace arangodb
