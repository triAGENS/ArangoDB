////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#ifndef TESTS_AQL_EXECUTORTESTHELPER_H
#define TESTS_AQL_EXECUTORTESTHELPER_H

#include "gtest/gtest.h"

#include "AqlItemBlockHelper.h"
#include "ExecutionBlockPipeline.h"
#include "MockTypedNode.h"
#include "Mocks/Servers.h"
#include "WaitingExecutionBlockMock.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemMatrix.h"
#include "Aql/BlockCollector.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionState.h"
#include "Aql/ExecutionStats.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Logger/LogMacros.h"

#include <numeric>
#include <tuple>

namespace arangodb {
namespace tests {
namespace aql {
/**
 * @brief Static helper class just offers helper methods
 * Do never instantiate
 *
 */
class asserthelper {
 private:
  asserthelper() {}

 public:
  static auto AqlValuesAreIdentical(AqlValue const& lhs, AqlValue const& rhs) -> bool;

  static auto RowsAreIdentical(SharedAqlItemBlockPtr actual, size_t actualRow,
                               SharedAqlItemBlockPtr expected, size_t expectedRow,
                               std::optional<std::vector<RegisterId>> const& onlyCompareRegisters = std::nullopt)
      -> bool;

  static auto ValidateAqlValuesAreEqual(SharedAqlItemBlockPtr actual,
                                        size_t actualRow, RegisterId actualRegister,
                                        SharedAqlItemBlockPtr expected, size_t expectedRow,
                                        RegisterId expectedRegister) -> void;

  static auto ValidateBlocksAreEqual(
      SharedAqlItemBlockPtr actual, SharedAqlItemBlockPtr expected,
      std::optional<std::vector<RegisterId>> const& onlyCompareRegisters = std::nullopt)
      -> void;

  static auto ValidateBlocksAreEqualUnordered(
      SharedAqlItemBlockPtr actual, SharedAqlItemBlockPtr expected,
      std::size_t numRowsNotContained = 0,
      std::optional<std::vector<RegisterId>> const& onlyCompareRegisters = std::nullopt)
      -> void;

  static auto ValidateBlocksAreEqualUnordered(
      SharedAqlItemBlockPtr actual, SharedAqlItemBlockPtr expected,
      std::unordered_set<size_t>& matchedRows, std::size_t numRowsNotContained = 0,
      std::optional<std::vector<RegisterId>> const& onlyCompareRegisters = std::nullopt)
      -> void;
};

template <std::size_t inputColumns = 1, std::size_t outputColumns = 1>
struct ExecutorTestHelper {
  using SplitType = std::variant<std::vector<std::size_t>, std::size_t, std::monostate>;

  ExecutorTestHelper(ExecutorTestHelper const&) = delete;
  ExecutorTestHelper(ExecutorTestHelper&&) = delete;
  explicit ExecutorTestHelper(Query& query, AqlItemBlockManager& itemBlockManager)
      : _expectedSkip{},
        _expectedState{ExecutionState::HASMORE},
        _testStats{false},
        _unorderedOutput{false},
        _appendEmptyBlock{false},
        _unorderedSkippedRows{0},
        _query(query),
        _itemBlockManager(itemBlockManager),
        _dummyNode{std::make_unique<SingletonNode>(
            const_cast<ExecutionPlan*>(
                _query.rootEngine()->root()->getPlanNode()->plan()),
            ExecutionNodeId{42})} {}

  auto setCallStack(AqlCallStack stack) -> ExecutorTestHelper& {
    _callStack = stack;
    return *this;
  }

  auto setCall(AqlCall c) -> ExecutorTestHelper& {
    _clientCall = AqlCallList{c};
    return *this;
  }

  auto setCallList(AqlCallList c) -> ExecutorTestHelper& {
    _clientCall = c;
    return *this;
  }


  auto setInputValue(MatrixBuilder<inputColumns> in) -> ExecutorTestHelper& {
    _input = std::move(in);
    return *this;
  }

  template <typename... Ts>
  auto setInputValueList(Ts&&... ts) -> ExecutorTestHelper& {
    _input = MatrixBuilder<inputColumns>{{ts}...};
    return *this;
  }

  auto setInputFromRowNum(size_t rows) -> ExecutorTestHelper& {
    static_assert(inputColumns == 1);
    _input.clear();
    for (auto i = size_t{0}; i < rows; ++i) {
      _input.emplace_back(RowBuilder<1>{static_cast<int>(i)});
    }
    return *this;
  }

  auto setInputSplit(std::vector<std::size_t> const& list) -> ExecutorTestHelper& {
    _inputSplit = list;
    return *this;
  }

  auto setInputSplitStep(std::size_t step) -> ExecutorTestHelper& {
    _inputSplit = step;
    return *this;
  }

  auto setInputSplitType(SplitType split) -> ExecutorTestHelper& {
    _inputSplit = split;
    return *this;
  }

  template <typename T>
  auto setOutputSplit(T&& list) -> ExecutorTestHelper& {
    ASSERT_FALSE(true);
    _outputSplit = std::forward<T>(list);
    return *this;
  }

  auto setTesteeNodeType(ExecutionNode::NodeType nodeType) -> ExecutorTestHelper& {
    _testeeNodeType = nodeType;
    return *this;
  }

  auto setWaitingBehaviour(WaitingExecutionBlockMock::WaitingBehaviour waitingBehaviour)
      -> ExecutorTestHelper& {
    _waitingBehaviour = waitingBehaviour;
    return *this;
  }

  auto expectOutput(std::array<RegisterId, outputColumns> const& regs,
                    MatrixBuilder<outputColumns> const& out,
                    std::vector<std::pair<size_t, uint64_t>> const& shadowRows = {})
      -> ExecutorTestHelper& {
    _outputRegisters = regs;
    _output = out;
    _outputShadowRows = shadowRows;
    return *this;
  }

  template <typename... Ts>
  auto expectOutputValueList(Ts&&... ts) -> ExecutorTestHelper& {
    static_assert(outputColumns == 1);
    _outputRegisters[0] = 1;
    _output = MatrixBuilder<outputColumns>{{ts}...};
    return *this;
  }

  /**
   * @brief
   *
   * @tparam Ts numeric type, can actually only be size_t
   * @param skipOnLevel List of skip counters returned per level. subquery skips first, the last entry is the skip on the executor
   * @return ExecutorTestHelper& chaining!
   */
  template <typename T, typename... Ts>
  auto expectSkipped(T skipFirst, Ts const... skipOnHigherLevel) -> ExecutorTestHelper& {
    _expectedSkip = SkipResult{};
    // This is obvious, proof: Homework.
    (_expectedSkip.didSkip(static_cast<size_t>(skipFirst)), ...,
     (_expectedSkip.incrementSubquery(),
      _expectedSkip.didSkip(static_cast<size_t>(skipOnHigherLevel))));

    // NOTE: the above will increment didSkip by the first entry.
    // For all following entries it will first increment the subquery depth
    // and then add the didSkip on them.
    return *this;
  }

  auto expectedState(ExecutionState state) -> ExecutorTestHelper& {
    _expectedState = state;
    return *this;
  }

  auto expectedStats(ExecutionStats stats) -> ExecutorTestHelper& {
    _expectedStats = stats;
    _testStats = true;
    return *this;
  }

  auto allowAnyOutputOrder(bool expected, size_t skippedRows = 0) -> ExecutorTestHelper& {
    _unorderedOutput = expected;
    _unorderedSkippedRows = skippedRows;
    return *this;
  }

  /**
   * @brief Add a dependency, i.e. add an ExecutionBlock to the *end* of the execution pipeline
   *
   * @tparam E The executor template parameter
   * @param executorInfos to build the executor
   * @param nodeType The type of executor node, only used for debug printing, defaults to SINGLETON
   * @return ExecutorTestHelper&
   */
  template <typename E>
  auto addDependency(RegisterInfos registerInfos, typename E::Infos executorInfos,
                     ExecutionNode::NodeType nodeType = ExecutionNode::SINGLETON)
      -> ExecutorTestHelper& {
    _pipeline.addDependency(createExecBlock<E>(std::move(registerInfos),
                                               std::move(executorInfos), nodeType));
    return *this;
  }

  /**
   * @brief Add a consumer, i.e. add an ExecutionBlock to the *beginning* of the execution pipeline
   *
   * @tparam E The executor template parameter
   * @param executorInfos to build the executor
   * @param nodeType The type of executor node, only used for debug printing, defaults to SINGLETON
   * @return ExecutorTestHelper&
   */
  template <typename E>
  auto addConsumer(RegisterInfos registerInfos, typename E::Infos executorInfos,
                   ExecutionNode::NodeType nodeType = ExecutionNode::SINGLETON)
      -> ExecutorTestHelper& {
    _pipeline.addConsumer(createExecBlock<E>(std::move(registerInfos),
                                             std::move(executorInfos), nodeType));
    return *this;
  }

  /**
   * @brief This appends an empty block after the input fully created.
   *        It simulates a situation where the Producer lies about the
   *        the last input with HASMORE, but it actually is not able
   *        to produce more.
   *
   * @param append If this should be enabled or not
   * @return ExecutorTestHelper& this for chaining
   */
  auto appendEmptyBlock(bool append) -> ExecutorTestHelper& {
    _appendEmptyBlock = append;
    return *this;
  }

  auto run(bool const loop = false) -> void {
    ResourceMonitor monitor;

    auto inputBlock = generateInputRanges(_itemBlockManager);

    auto skippedTotal = SkipResult{};
    auto finalState = ExecutionState::HASMORE;

    TRI_ASSERT(!_pipeline.empty());

    _pipeline.addDependency(std::move(inputBlock));

    BlockCollector allResults{&_itemBlockManager};

    if (!loop) {
      auto const [state, skipped, result] = _pipeline.get().front()->execute(_callStack, _clientCall);
      skippedTotal.merge(skipped, false);
      finalState = state;
      if (result != nullptr) {
        allResults.add(result);
      }
    } else {
      do {
        auto const [state, skipped, result] = _pipeline.get().front()->execute(_callStack, _clientCall);
        finalState = state;
        skippedTotal.merge(skipped, false);
        auto& call = _clientCall.modifyNextCall();
        call.didSkip(skipped.getSkipCount());
        if (result != nullptr) {
          call.didProduce(result->size());
          allResults.add(result);
        }
        call.resetSkipCount();
      } while (finalState != ExecutionState::DONE &&
               (!_clientCall.peekNextCall().hasSoftLimit() ||
                (_clientCall.peekNextCall().getLimit() + _clientCall.peekNextCall().getOffset()) > 0));
    }
    EXPECT_EQ(skippedTotal, _expectedSkip);
    EXPECT_EQ(finalState, _expectedState);
    SharedAqlItemBlockPtr result = allResults.steal();
    if (result == nullptr) {
      // Empty output, possible if we skip all
      EXPECT_EQ(_output.size(), 0)
          << "Executor does not yield output, although it is expected";
    } else {
      SharedAqlItemBlockPtr expectedOutputBlock =
          buildBlock<outputColumns>(_itemBlockManager, std::move(_output), _outputShadowRows);
      std::vector<RegisterId> outRegVector(_outputRegisters.begin(),
                                           _outputRegisters.end());
      if (_unorderedOutput) {
        asserthelper::ValidateBlocksAreEqualUnordered(result, expectedOutputBlock,
                                                      _unorderedSkippedRows, outRegVector);
      } else {
        asserthelper::ValidateBlocksAreEqual(result, expectedOutputBlock, outRegVector);
      }
    }

    if (_testStats) {
      ExecutionStats actualStats;
      _query.rootEngine()->collectExecutionStats(actualStats);
      // simon: engine does not collect most block stats
      for (auto const& block : _pipeline.get()) {
        block->collectExecStats(actualStats);
      }
      EXPECT_EQ(actualStats, _expectedStats);
    }
  };

 private:
  /**
   * @brief create an ExecutionBlock without tying it into the pipeline.
   *
   * @tparam E The executor template parameter
   * @param executorInfos to build the executor
   * @param nodeType The type of executor node, only used for debug printing, defaults to SINGLETON
   * @return ExecBlock
   *
   * Now private to prevent us from leaking memory
   */
  template <typename E>
  auto createExecBlock(RegisterInfos registerInfos, typename E::Infos executorInfos,
                       ExecutionNode::NodeType nodeType = ExecutionNode::SINGLETON)
      -> ExecBlock {
    auto& testeeNode = _execNodes.emplace_back(
        std::make_unique<MockTypedNode>(_query.plan(),
                                        ExecutionNodeId{_execNodes.size()}, nodeType));
    return std::make_unique<ExecutionBlockImpl<E>>(_query.rootEngine(),
                                                   testeeNode.get(), std::move(registerInfos),
                                                   std::move(executorInfos));
  }

  auto generateInputRanges(AqlItemBlockManager& itemBlockManager)
      -> std::unique_ptr<ExecutionBlock> {
    using VectorSizeT = std::vector<std::size_t>;

    MatrixBuilder<inputColumns> matrix;

    std::deque<SharedAqlItemBlockPtr> blockDeque;

    std::optional<VectorSizeT::iterator> iter, end;

    if (std::holds_alternative<VectorSizeT>(_inputSplit)) {
      iter = std::get<VectorSizeT>(_inputSplit).begin();
      end = std::get<VectorSizeT>(_inputSplit).end();
    }

    for (auto const& value : _input) {
      matrix.push_back(value);

      TRI_ASSERT(!_inputSplit.valueless_by_exception());

      bool openNewBlock =
          std::visit(overload{[&](VectorSizeT& list) {
                                if (*iter != *end && matrix.size() == **iter) {
                                  iter->operator++();
                                  return true;
                                }

                                return false;
                              },
                              [&](std::size_t size) {
                                return matrix.size() == size;
                              },
                              [](auto) { return false; }},
                     _inputSplit);
      if (openNewBlock) {
        SharedAqlItemBlockPtr inputBlock =
            buildBlock<inputColumns>(itemBlockManager, std::move(matrix));
        blockDeque.emplace_back(inputBlock);
        matrix.clear();
      }
    }

    if (!matrix.empty()) {
      SharedAqlItemBlockPtr inputBlock =
          buildBlock<inputColumns>(itemBlockManager, std::move(matrix));
      blockDeque.emplace_back(inputBlock);
    }
    if (_appendEmptyBlock) {
      blockDeque.emplace_back(nullptr);
    }

    return std::make_unique<WaitingExecutionBlockMock>(_query.rootEngine(),
                                                       _dummyNode.get(),
                                                       std::move(blockDeque),
                                                       _waitingBehaviour);
  }


  // Initialize without a outer query.
  AqlCallStack _callStack{};
  // Default initialize with a fetchAll call.
  AqlCallList _clientCall{AqlCall{}};
  MatrixBuilder<inputColumns> _input;
  MatrixBuilder<outputColumns> _output;
  std::vector<std::pair<size_t, uint64_t>> _outputShadowRows{};
  std::array<RegisterId, outputColumns> _outputRegisters;
  SkipResult _expectedSkip;
  ExecutionState _expectedState;
  ExecutionStats _expectedStats;
  bool _testStats;
  ExecutionNode::NodeType _testeeNodeType{ExecutionNode::MAX_NODE_TYPE_VALUE};
  WaitingExecutionBlockMock::WaitingBehaviour _waitingBehaviour =
      WaitingExecutionBlockMock::NEVER;
  bool _unorderedOutput;
  bool _appendEmptyBlock;
  std::size_t _unorderedSkippedRows;

  SplitType _inputSplit = {std::monostate()};
  SplitType _outputSplit = {std::monostate()};

  arangodb::aql::Query& _query;
  arangodb::aql::AqlItemBlockManager& _itemBlockManager;
  std::unique_ptr<arangodb::aql::ExecutionNode> _dummyNode;
  Pipeline _pipeline;
  std::vector<std::unique_ptr<MockTypedNode>> _execNodes;
};

}  // namespace aql
}  // namespace tests
}  // namespace arangodb

#endif  // TESTS_AQL_EXECUTORTESTHELPER_H
