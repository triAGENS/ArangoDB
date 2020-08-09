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

#include "ExecutionBlockImpl.h"

#include "Aql/AllRowsFetcher.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/CalculationExecutor.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ConstrainedSortExecutor.h"
#include "Aql/CountCollectExecutor.h"
#include "Aql/DistinctCollectExecutor.h"
#include "Aql/EnumerateCollectionExecutor.h"
#include "Aql/EnumerateListExecutor.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionState.h"
#include "Aql/FilterExecutor.h"
#include "Aql/HashedCollectExecutor.h"
#include "Aql/IResearchViewExecutor.h"
#include "Aql/IdExecutor.h"
#include "Aql/IndexExecutor.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/KShortestPathsExecutor.h"
#include "Aql/LimitExecutor.h"
#include "Aql/MaterializeExecutor.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/MultiDependencySingleRowFetcher.h"
#include "Aql/NoResultsExecutor.h"
#include "Aql/ParallelUnsortedGatherExecutor.h"
#include "Aql/Query.h"
#include "Aql/QueryOptions.h"
#include "Aql/RegisterInfos.h"
#include "Aql/ReturnExecutor.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Aql/ShortestPathExecutor.h"
#include "Aql/SimpleModifier.h"
#include "Aql/SingleRemoteModificationExecutor.h"
#include "Aql/SkipResult.h"
#include "Aql/SortExecutor.h"
#include "Aql/SortRegister.h"
#include "Aql/SortedCollectExecutor.h"
#include "Aql/SortingGatherExecutor.h"
#include "Aql/SubqueryEndExecutor.h"
#include "Aql/SubqueryExecutor.h"
#include "Aql/SubqueryStartExecutor.h"
#include "Aql/TraversalExecutor.h"
#include "Aql/UnsortedGatherExecutor.h"
#include "Aql/UpsertModifier.h"
#include "Basics/system-functions.h"
#include "Transaction/Context.h"

#include <velocypack/Dumper.h>
#include <velocypack/velocypack-aliases.h>

#include <boost/core/demangle.hpp>

#include <type_traits>

using namespace arangodb;
using namespace arangodb::aql;

#define LOG_QUERY(logId, level)            \
  LOG_TOPIC(logId, level, Logger::QUERIES) \
      << "[query#" << this->_engine->getQuery().id() << "] "

/*
 * Creates a metafunction `checkName` that tests whether a class has a method
 * named `methodName`, used like this:
 *
 * CREATE_HAS_MEMBER_CHECK(someMethod, hasSomeMethod);
 * ...
 * constexpr bool someClassHasSomeMethod = hasSomeMethod<SomeClass>::value;
 */

#define CREATE_HAS_MEMBER_CHECK(methodName, checkName)               \
  template <typename T>                                              \
  class checkName {                                                  \
    template <typename C>                                            \
    static std::true_type test(decltype(&C::methodName));            \
    template <typename C>                                            \
    static std::true_type test(decltype(&C::template methodName<>)); \
    template <typename>                                              \
    static std::false_type test(...);                                \
                                                                     \
   public:                                                           \
    static constexpr bool value = decltype(test<T>(0))::value;       \
  }

CREATE_HAS_MEMBER_CHECK(initializeCursor, hasInitializeCursor);
CREATE_HAS_MEMBER_CHECK(expectedNumberOfRows, hasExpectedNumberOfRows);
CREATE_HAS_MEMBER_CHECK(skipRowsRange, hasSkipRowsRange);
CREATE_HAS_MEMBER_CHECK(expectedNumberOfRowsNew, hasExpectedNumberOfRowsNew);

#ifdef ARANGODB_USE_GOOGLE_TESTS
// Forward declaration of Test Executors.
// only used as long as isNewStyleExecutor is required.
namespace arangodb::aql {
class TestLambdaSkipExecutor;
}  // namespace arangodb::aql
#endif

/*
 *  Determine whether an executor cannot bypass subquery skips.
 *  This is if exection of this Executor does have side-effects
 *  other then it's own result.
 */

template <typename Executor>
constexpr bool executorHasSideEffects =
    is_one_of_v<Executor, ModificationExecutor<AllRowsFetcher, InsertModifier>,
                ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, InsertModifier>,
                ModificationExecutor<AllRowsFetcher, RemoveModifier>,
                ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, RemoveModifier>,
                ModificationExecutor<AllRowsFetcher, UpdateReplaceModifier>,
                ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpdateReplaceModifier>,
                ModificationExecutor<AllRowsFetcher, UpsertModifier>,
                ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpsertModifier>, SubqueryExecutor<true>>;

template <class Executor>
ExecutionBlockImpl<Executor>::ExecutionBlockImpl(ExecutionEngine* engine,
                                                 ExecutionNode const* node,
                                                 RegisterInfos registerInfos,
                                                 typename Executor::Infos executorInfos)
    : ExecutionBlock(engine, node),
      _registerInfos(std::move(registerInfos)),
      _dependencyProxy(_dependencies, _registerInfos.numberOfInputRegisters()),
      _rowFetcher(_dependencyProxy),
      _executorInfos(std::move(executorInfos)),
      _executor(_rowFetcher, _executorInfos),
      _outputItemRow(),
      _query(engine->getQuery()),
      _state(InternalState::FETCH_DATA),
      _execState{ExecState::CHECKCALL},
      _lastRange{ExecutorState::HASMORE},
      _upstreamRequest{},
      _clientRequest{},
      _stackBeforeWaiting{},
      _hasUsedDataRangeBlock{false} {
}

template <class Executor>
ExecutionBlockImpl<Executor>::~ExecutionBlockImpl() = default;

template <class Executor>
std::unique_ptr<OutputAqlItemRow> ExecutionBlockImpl<Executor>::createOutputRow(
    SharedAqlItemBlockPtr&& newBlock, AqlCall&& call) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (newBlock != nullptr) {
    // Assert that the block has enough registers. This must be guaranteed by
    // the register planning.
    TRI_ASSERT(newBlock->getNrRegs() == _registerInfos.numberOfOutputRegisters());
    // Check that all output registers are empty.
    size_t const n = newBlock->size();
    auto const& regs = _registerInfos.getOutputRegisters();
    if (!regs.empty()) {
      bool const hasShadowRows = newBlock->hasShadowRows();
      for (size_t row = 0; row < n; row++) {
        if (!hasShadowRows || !newBlock->isShadowRow(row)) {
          for (auto const& reg : regs) {
            AqlValue const& val = newBlock->getValueReference(row, reg);
            TRI_ASSERT(val.isEmpty());
          }
        }
      }
    }
  }
#endif

  constexpr auto copyRowBehaviour = [] {
    if constexpr (Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable) {
      return OutputAqlItemRow::CopyRowBehavior::DoNotCopyInputRows;
    } else {
      return OutputAqlItemRow::CopyRowBehavior::CopyInputRows;
    }
  }();

  return std::make_unique<OutputAqlItemRow>(std::move(newBlock),
                                            registerInfos().getOutputRegisters(),
                                            registerInfos().registersToKeep(),
                                            registerInfos().registersToClear(),
                                            call, copyRowBehaviour);
}

template <class Executor>
Executor& ExecutionBlockImpl<Executor>::executor() {
  return _executor;
}

template <class Executor>
QueryContext const& ExecutionBlockImpl<Executor>::getQuery() const {
  return _query;
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executorInfos() const -> ExecutorInfos const& {
  return _executorInfos;
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::registerInfos() const -> RegisterInfos const& {
  return _registerInfos;
}

namespace arangodb::aql {

template <bool customInit>
struct InitializeCursor {};

template <>
struct InitializeCursor<false> {
  template <class Executor>
  static void init(Executor& executor, typename Executor::Fetcher& rowFetcher,
                   typename Executor::Infos& infos) {
    // destroy and re-create the Executor
    executor.~Executor();
    new (&executor) Executor(rowFetcher, infos);
  }
};

template <>
struct InitializeCursor<true> {
  template <class Executor>
  static void init(Executor& executor, typename Executor::Fetcher&,
                   typename Executor::Infos&) {
    // re-initialize the Executor
    executor.initializeCursor();
  }
};
}  // namespace arangodb::aql

template <class Executor>
std::pair<ExecutionState, Result> ExecutionBlockImpl<Executor>::initializeCursor(InputAqlItemRow const& input) {
  // reinitialize the DependencyProxy
  _dependencyProxy.reset();
  _hasUsedDataRangeBlock = false;
  initOnce();
  // destroy and re-create the Fetcher
  _rowFetcher.~Fetcher();
  new (&_rowFetcher) Fetcher(_dependencyProxy);

  if constexpr (isMultiDepExecutor<Executor>) {
    _lastRange.reset();
    _rowFetcher.init();
  } else {
    _lastRange = DataRange(ExecutorState::HASMORE);
  }

  TRI_ASSERT(_skipped.nothingSkipped());
  _skipped.reset();
  TRI_ASSERT(_state == InternalState::DONE || _state == InternalState::FETCH_DATA);
  _state = InternalState::FETCH_DATA;

  resetExecutor();

  // // use this with c++17 instead of specialization below
  // if constexpr (std::is_same_v<Executor, IdExecutor>) {
  //   if (items != nullptr) {
  //     _executor._inputRegisterValues.reset(
  //         items->slice(pos, *(_executor._registerInfos.registersToKeep())));
  //   }
  // }

  return ExecutionBlock::initializeCursor(input);
}

template <class Executor>
std::pair<ExecutionState, Result> ExecutionBlockImpl<Executor>::shutdown(int errorCode) {
  return ExecutionBlock::shutdown(errorCode);
}

template <class Executor>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
ExecutionBlockImpl<Executor>::execute(AqlCallStack const& stack, AqlCallList clientCall) {
  traceExecuteBegin(stack, clientCall);
  // silence tests -- we need to introduce new failure tests for fetchers
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome1") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  initOnce();
  auto res = executeWithoutTrace(stack, std::move(clientCall));
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto const& [state, skipped, block] = res;
  if (block != nullptr) {
    block->validateShadowRowConsistency();
  }
#endif
  traceExecuteEnd(res);
  return res;
}

// Work around GCC bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56480
// Without the namespaces it fails with
// error: specialization of 'template<class Executor> std::pair<arangodb::aql::ExecutionState, arangodb::Result> arangodb::aql::ExecutionBlockImpl<Executor>::initializeCursor(arangodb::aql::AqlItemBlock*, size_t)' in different namespace
namespace arangodb::aql {
// TODO -- remove this specialization when cpp 17 becomes available

template <>
template <>
auto ExecutionBlockImpl<IdExecutor<ConstFetcher>>::injectConstantBlock<IdExecutor<ConstFetcher>>(
    SharedAqlItemBlockPtr block, SkipResult skipped) -> void {
  // reinitialize the DependencyProxy
  _dependencyProxy.reset();

  // destroy and re-create the Fetcher
  _rowFetcher.~Fetcher();
  new (&_rowFetcher) Fetcher(_dependencyProxy);

  TRI_ASSERT(_skipped.nothingSkipped());

  // Local skipped is either fresh (depth == 1)
  // Or exactly of the size handed in
  TRI_ASSERT(_skipped.subqueryDepth() == 1 ||
             _skipped.subqueryDepth() == skipped.subqueryDepth());

  TRI_ASSERT(_state == InternalState::DONE || _state == InternalState::FETCH_DATA);

  _state = InternalState::FETCH_DATA;

  // Reset state of execute
  _lastRange = AqlItemBlockInputRange{ExecutorState::HASMORE};
  _hasUsedDataRangeBlock = false;
  _upstreamState = ExecutionState::HASMORE;

  _rowFetcher.injectBlock(std::move(block), std::move(skipped));

  resetExecutor();
}

// TODO -- remove this specialization when cpp 17 becomes available
template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<IdExecutor<ConstFetcher>>::initializeCursor(
    InputAqlItemRow const& input) {
  SharedAqlItemBlockPtr block =
      input.cloneToBlock(_engine->itemBlockManager(),
                         registerInfos().registersToKeep().back(),
                         registerInfos().numberOfOutputRegisters());
  TRI_ASSERT(_skipped.nothingSkipped());
  _skipped.reset();
  // We inject an empty copy of our skipped here,
  // This is resetted, but will maintain the size
  injectConstantBlock(std::move(block), _skipped);

  // end of default initializeCursor
  return ExecutionBlock::initializeCursor(input);
}

// TODO the shutdown specializations shall be unified!

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<TraversalExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result result;

  std::tie(state, result) = ExecutionBlock::shutdown(errorCode);

  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  return this->executor().shutdown(errorCode);
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<ShortestPathExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result result;

  std::tie(state, result) = ExecutionBlock::shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  return this->executor().shutdown(errorCode);
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<KShortestPathsExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result result;

  std::tie(state, result) = ExecutionBlock::shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  return this->executor().shutdown(errorCode);
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<SubqueryExecutor<true>>::shutdown(int errorCode) {
  ExecutionState state;
  Result subqueryResult;
  // shutdown is repeatable
  std::tie(state, subqueryResult) = this->executor().shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {ExecutionState::WAITING, subqueryResult};
  }
  Result result;

  std::tie(state, result) = ExecutionBlock::shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  if (result.fail()) {
    return {state, result};
  }
  return {state, subqueryResult};
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<SubqueryExecutor<false>>::shutdown(int errorCode) {
  ExecutionState state;
  Result subqueryResult;
  // shutdown is repeatable
  std::tie(state, subqueryResult) = this->executor().shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {ExecutionState::WAITING, subqueryResult};
  }
  Result result;

  std::tie(state, result) = ExecutionBlock::shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  if (result.fail()) {
    return {state, result};
  }
  return {state, subqueryResult};
}

template <>
std::pair<ExecutionState, Result>
ExecutionBlockImpl<IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>::shutdown(int errorCode) {
  if (this->executorInfos().isResponsibleForInitializeCursor()) {
    return ExecutionBlock::shutdown(errorCode);
  }
  return {ExecutionState::DONE, {errorCode}};
}

}  // namespace arangodb::aql

// TODO: We need to define the size of this block based on Input / Executor / Subquery depth
template <class Executor>
auto ExecutionBlockImpl<Executor>::allocateOutputBlock(AqlCall&& call, DataRange const& inputRange)
    -> std::unique_ptr<OutputAqlItemRow> {
  if constexpr (Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable) {
    // Passthrough variant, re-use the block stored in InputRange
    if (!_hasUsedDataRangeBlock) {
      // In the pass through variant we have the contract that we work on a
      // block all or nothing, so if we have used the block once, we cannot use it again
      // however we cannot remove the _lastRange as it may contain additional information.
      _hasUsedDataRangeBlock = true;
      return createOutputRow(SharedAqlItemBlockPtr(_lastRange.getBlock()), std::move(call));
    }

    return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
  } else {
    if constexpr (isMultiDepExecutor<Executor>) {
      // MultiDepExecutor would require dependency handling.
      // We do not have it here.
      if (!inputRange.hasValidRow()) {
        // On empty input do not yet create output.
        // We are going to ask again later
        return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
      }
    } else {
      if (!inputRange.hasValidRow() && inputRange.upstreamState() == ExecutorState::HASMORE) {
        // On empty input do not yet create output.
        // We are going to ask again later
        return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
      }
    }

    // Non-Passthrough variant, we need to allocate the block ourselfs
    size_t blockSize = ExecutionBlock::DefaultBatchSize;
    if constexpr (hasExpectedNumberOfRowsNew<Executor>::value) {
      // Only limit the output size if there will be no more
      // data from upstream. Or if we have ordered a SOFT LIMIT.
      // Otherwise we will overallocate here.
      // In production it is now very unlikely in the non-softlimit case
      // that the upstream is no block using less than batchSize many rows, but returns HASMORE.
      if (inputRange.upstreamState() == ExecutorState::DONE || call.hasSoftLimit()) {
        blockSize = _executor.expectedNumberOfRowsNew(inputRange, call);
        if (inputRange.upstreamState() == ExecutorState::HASMORE) {
          // There might be more from above!
          blockSize = std::max(call.getLimit(), blockSize);
        }

        auto const numShadowRows = inputRange.countShadowRows();
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        // The executor cannot expect to produce more then the limit!
        if constexpr (!std::is_same_v<Executor, SubqueryStartExecutor>) {
          // Except the subqueryStartExecutor, it's limit differs
          // from it's output (it needs to count the new ShadowRows in addition)
          // This however is only correct, as long as we are in no subquery context
          if (numShadowRows == 0) {
            TRI_ASSERT(blockSize <= call.getLimit());
          }
        }
#endif

        blockSize += numShadowRows;
        // We have an upper bound by DefaultBatchSize;
        blockSize = std::min(ExecutionBlock::DefaultBatchSize, blockSize);
      }
    }

    if (blockSize == 0) {
      // There is no data to be produced
      return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
    }
    return createOutputRow(_engine->itemBlockManager().requestBlock(
                               blockSize, _registerInfos.numberOfOutputRegisters()),
                           std::move(call));
  }
}

template <class Executor>
void ExecutionBlockImpl<Executor>::ensureOutputBlock(AqlCall&& call,
                                                     DataRange const& inputRange) {
  if (_outputItemRow == nullptr || !_outputItemRow->isInitialized()) {
    _outputItemRow = allocateOutputBlock(std::move(call), inputRange);
  } else {
    _outputItemRow->setCall(std::move(call));
  }
}

// This cannot return upstream call or shadowrows.
template <class Executor>
auto ExecutionBlockImpl<Executor>::nextState(AqlCall const& call) const -> ExecState {
  if (_executorReturnedDone) {
    return ExecState::FASTFORWARD;
  }
  if (call.getOffset() > 0) {
    // First skip
    return ExecState::SKIP;
  }
  if (call.getLimit() > 0) {
    // Then produce
    return ExecState::PRODUCE;
  }
  if (call.hardLimit == 0) {
    // We reached hardLimit, fast forward
    return ExecState::FASTFORWARD;
  }
  // now we are done.
  return ExecState::DONE;
}

/// @brief request an AqlItemBlock from the memory manager
template <class Executor>
SharedAqlItemBlockPtr ExecutionBlockImpl<Executor>::requestBlock(size_t nrItems,
                                                                 RegisterCount nrRegs) {
  return _engine->itemBlockManager().requestBlock(nrItems, nrRegs);
}

//
// FETCHER:  if we have one output row per input row, we can skip
//           directly by just calling the fetcher and see whether
//           it produced any output.
//           With the new architecture we should be able to just skip
//           ahead on the input range, fetching new blocks when necessary
// EXECUTOR: the executor has a specialised skipRowsRange method
//           that will be called to skip
// SUBQUERY_START:
// SUBQUERY_END:
//
enum class SkipRowsRangeVariant {
  FETCHER,
  EXECUTOR,
  SUBQUERY_START,
  SUBQUERY_END
};

// This function is just copy&pasted from above to decide which variant of
// skip is used for which executor.
template <class Executor>
static SkipRowsRangeVariant constexpr skipRowsType() {
  bool constexpr useFetcher =
      Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable &&
      !std::is_same_v<Executor, SubqueryExecutor<true>>;

  bool constexpr useExecutor = hasSkipRowsRange<Executor>::value;

  static_assert(!std::is_same_v<Executor, SubqueryExecutor<true>> ||
                hasSkipRowsRange<Executor>::value);

  // ConstFetcher and SingleRowFetcher<BlockPassthrough::Enable> can skip, but
  // it may not be done for modification subqueries.
  static_assert(useFetcher ==
                    (std::is_same_v<typename Executor::Fetcher, ConstFetcher> ||
                     (std::is_same_v<typename Executor::Fetcher, SingleRowFetcher<BlockPassthrough::Enable>> &&
                      !std::is_same<Executor, SubqueryExecutor<true>>::value)),
                "Unexpected fetcher for SkipVariants::FETCHER");

  static_assert(
      useExecutor ==
          (is_one_of_v<
              Executor, FilterExecutor, ShortestPathExecutor, ReturnExecutor, KShortestPathsExecutor, ParallelUnsortedGatherExecutor,
              IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>, IdExecutor<ConstFetcher>,
              HashedCollectExecutor, IndexExecutor, EnumerateCollectionExecutor, DistinctCollectExecutor,
              ConstrainedSortExecutor, CountCollectExecutor, SubqueryExecutor<true>,
#ifdef ARANGODB_USE_GOOGLE_TESTS
              TestLambdaSkipExecutor,
#endif
              ModificationExecutor<AllRowsFetcher, InsertModifier>,
              ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, InsertModifier>,
              ModificationExecutor<AllRowsFetcher, RemoveModifier>,
              ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, RemoveModifier>,
              ModificationExecutor<AllRowsFetcher, UpdateReplaceModifier>,
              ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpdateReplaceModifier>,
              ModificationExecutor<AllRowsFetcher, UpsertModifier>,
              ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpsertModifier>, TraversalExecutor,
              EnumerateListExecutor, SubqueryStartExecutor, SubqueryEndExecutor, SortedCollectExecutor,
              LimitExecutor, UnsortedGatherExecutor, SortingGatherExecutor, SortExecutor,
              IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize>,
              IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize>,
              IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::Materialize>,
              IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize>,
              IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize>,
              IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::Materialize>,
              IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize>,
              IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize>,
              IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::Materialize>,
              IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize>,
              IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize>,
              IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::Materialize>,
              IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>,
              TraversalExecutor, EnumerateListExecutor, SubqueryStartExecutor, SubqueryEndExecutor, SortedCollectExecutor,
              LimitExecutor, NoResultsExecutor, SingleRemoteModificationExecutor<IndexTag>, SingleRemoteModificationExecutor<Insert>,
              SingleRemoteModificationExecutor<Remove>, SingleRemoteModificationExecutor<Update>,
              SingleRemoteModificationExecutor<Replace>, SingleRemoteModificationExecutor<Upsert>,
              MaterializeExecutor<RegisterId>, MaterializeExecutor<std::string const&>>),
      "Unexpected executor for SkipVariants::EXECUTOR");

  // The LimitExecutor will not work correctly with SkipVariants::FETCHER!
  static_assert(
      !std::is_same<Executor, LimitExecutor>::value || useFetcher,
      "LimitExecutor needs to implement skipRows() to work correctly");

  if constexpr (useExecutor) {
    return SkipRowsRangeVariant::EXECUTOR;
  } else {
    static_assert(useFetcher);
    return SkipRowsRangeVariant::FETCHER;
  }
}

// Let's do it the C++ way.
template <class T>
struct dependent_false : std::false_type {};

/**
 * @brief Define the variant of FastForward behaviour
 *
 * FULLCOUNT => Call executeSkipRowsRange and report what has been skipped.
 * EXECUTOR => Call executeSkipRowsRange, but do not report what has been skipped.
 *             (This instance is used to make sure Modifications are performed, or stats are correct)
 * FETCHER => Do not bother the Executor, drop all from input, without further reporting
 */
enum class FastForwardVariant { FULLCOUNT, EXECUTOR, FETCHER };

template <class Executor>
static auto fastForwardType(AqlCall const& call, Executor const& e) -> FastForwardVariant {
  if (call.needsFullCount() && call.getOffset() == 0 && call.getLimit() == 0) {
    // Only start fullCount after the original call is fulfilled. Otherwise
    // do fast-forward variant
    TRI_ASSERT(call.hasHardLimit());
    return FastForwardVariant::FULLCOUNT;
  }
  // TODO: We only need to do this if the executor is required to call.
  // e.g. Modifications and SubqueryStart will always need to be called. Limit only if it needs to report fullCount
  if constexpr (is_one_of_v<Executor, LimitExecutor, SubqueryStartExecutor, SubqueryExecutor<true>> ||
                executorHasSideEffects<Executor>) {
    return FastForwardVariant::EXECUTOR;
  }
  return FastForwardVariant::FETCHER;
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executeFetcher(AqlCallStack const& stack,
                                                  AqlCallType const& aqlCall,
                                                  ADB_IGNORE_UNUSED bool wasCalledWithContinueCall)
    -> std::tuple<ExecutionState, SkipResult, typename Fetcher::DataRange> {
  // TODO The logic in the MultiDependencySingleRowFetcher branch should be
  //      moved into the MultiDependencySingleRowFetcher.
  static_assert(isMultiDepExecutor<Executor> ==
                std::is_same_v<Fetcher, MultiDependencySingleRowFetcher>);
  if constexpr (std::is_same_v<Fetcher, MultiDependencySingleRowFetcher>) {
    static_assert(
        !executorHasSideEffects<Executor>,
        "there is a special implementation for side-effect executors to "
        "exchange the stack. For the MultiDependencyFetcher this special "
        "case is not implemented. There is no reason to disallow this "
        "case here however, it is just not needed thus far.");
    // Note the aqlCall is an AqlCallSet in this case:
    static_assert(std::is_same_v<AqlCallSet, std::decay_t<decltype(aqlCall)>>);
    TRI_ASSERT(_lastRange.numberDependencies() == _dependencies.size());
    auto const& [state, skipped, ranges] = _rowFetcher.execute(stack, aqlCall);
    for (auto const& [dependency, range] : ranges) {
      _lastRange.setDependency(dependency, range);
    }
    return {state, skipped, _lastRange};

  } else {
    static_assert(std::is_same_v<AqlCall, std::decay_t<decltype(aqlCall)>>);
    auto res =
        _rowFetcher.execute(stack, std::move(createUpstreamCall(std::move(aqlCall),
                                                                wasCalledWithContinueCall)));
    if constexpr (executorHasSideEffects<Executor>) {
      // Just make sure we did not Skip anything
      TRI_ASSERT(std::get<SkipResult>(res).nothingSkipped());
    }
    return res;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executeProduceRows(typename Fetcher::DataRange& input,
                                                      OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, typename Executor::Stats, AqlCallType> {
  if constexpr (isMultiDepExecutor<Executor>) {
    TRI_ASSERT(input.numberDependencies() == _dependencies.size());
    return _executor.produceRows(input, output);
  } else if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
    // The SubqueryExecutor has it's own special handling outside.
    // SO this code is in fact not reachable
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
  } else {
    auto [state, stats, call] = _executor.produceRows(input, output);
    return {state, stats, call};
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executeSkipRowsRange(typename Fetcher::DataRange& inputRange,
                                                        AqlCall& call)
    -> std::tuple<ExecutorState, typename Executor::Stats, size_t, AqlCallType> {
  // The skippedRows is a temporary counter used in this function
  // We need to make sure to reset it afterwards.
  TRI_DEFER(call.resetSkipCount());
  if constexpr (skipRowsType<Executor>() == SkipRowsRangeVariant::EXECUTOR) {
    if constexpr (isMultiDepExecutor<Executor>) {
      TRI_ASSERT(inputRange.numberDependencies() == _dependencies.size());
      // If the executor has a method skipRowsRange, to skip outputs.
      // Every non-passthrough executor needs to implement this.
      auto res = _executor.skipRowsRange(inputRange, call);
      _executorReturnedDone = std::get<ExecutorState>(res) == ExecutorState::DONE;
      return res;
    } else if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
      // The SubqueryExecutor has it's own special handling outside.
      // SO this code is in fact not reachable
      TRI_ASSERT(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    } else {
      auto [state, stats, skipped, localCall] = _executor.skipRowsRange(inputRange, call);
      _executorReturnedDone = state == ExecutorState::DONE;
      return {state, stats, skipped, localCall};
    }
  } else if constexpr (skipRowsType<Executor>() == SkipRowsRangeVariant::FETCHER) {
    // If we know that every input row produces exactly one output row (this
    // is a property of the executor), then we can just let the fetcher skip
    // the number of rows that we would like to skip.
    // Returning this will trigger to end in upstream state now, with the
    // call that was handed it.
    static_assert(
        std::is_same_v<typename Executor::Stats, NoStats>,
        "Executors with custom statistics must implement skipRowsRange.");
    return {inputRange.upstreamState(), NoStats{}, 0, call};
  } else {
    static_assert(dependent_false<Executor>::value,
                  "This value of SkipRowsRangeVariant is not supported");
    TRI_ASSERT(false);
  }
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
}

template <>
template <>
auto ExecutionBlockImpl<SubqueryStartExecutor>::shadowRowForwardingSubqueryStart(AqlCallStack const& stack, AqlCall& subqueryCall)
    -> ExecState {
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());
  if (_lastRange.hasDataRow()) {
    // If we have a dataRow, the executor needs to write it's output.
    // If we get woken up by a dataRow during forwarding of ShadowRows
    // This will return false, and if so we need to call produce instead.
    auto didWrite = _executor.produceShadowRow(_lastRange, *_outputItemRow);
    // The Subquery Start returns DONE after every row.
    // This needs to be resetted as soon as a shadowRow has been produced
    _executorReturnedDone = false;
    // Need to report that we have written a row in the call

    if (didWrite) {
      subqueryCall.didProduce(1);
      if (_lastRange.hasShadowRow()) {
        // Forward the ShadowRows
        return ExecState::SHADOWROWS;
      }
      return ExecState::NEXTSUBQUERY;
    } else {
      // Woken up after shadowRow forwarding
      // Need to call the Executor
      return ExecState::CHECKCALL;
    }
  } else {
    // Need to forward the ShadowRows
    auto&& [state, shadowRow] = _lastRange.nextShadowRow();
    TRI_ASSERT(shadowRow.isInitialized());
    _outputItemRow->increaseShadowRowDepth(shadowRow);
    TRI_ASSERT(_outputItemRow->produced());
    _outputItemRow->advanceRow();

    if (_lastRange.hasShadowRow()) {
      return ExecState::SHADOWROWS;
    }
    // If we do not have more shadowRows
    // we need to return.

    auto& subqueryCallList = stack.getCallListAtDepth(shadowRow.getDepth());

    if (!subqueryCallList.hasDefaultCalls()) {
      return ExecState::DONE;
    }

    auto& subqueryCall = subqueryCallList.peekNextCall();
    if (subqueryCall.getLimit() == 0 && !subqueryCall.needSkipMore()) {
      return ExecState::DONE;
    }

    _executorReturnedDone = false;

    return ExecState::NEXTSUBQUERY;
  }
}

template <>
auto ExecutionBlockImpl<SubqueryEndExecutor>::shadowRowForwarding()
    -> ExecState {
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());
  if (!_lastRange.hasShadowRow()) {
    // We got back without a ShadowRow in the LastRange
    // Let client call again
    return ExecState::NEXTSUBQUERY;
  }
  auto&& [state, shadowRow] = _lastRange.nextShadowRow();
  TRI_ASSERT(shadowRow.isInitialized());
  if (shadowRow.isRelevant()) {
    // We need to consume the row, and write the Aggregate to it.
    _executor.consumeShadowRow(shadowRow, *_outputItemRow);
    // we need to reset the ExecutorHasReturnedDone, it will
    // return done after every subquery is fully collected.
    _executorReturnedDone = false;

  } else {
    _outputItemRow->decreaseShadowRowDepth(shadowRow);
  }

  TRI_ASSERT(_outputItemRow->produced());
  _outputItemRow->advanceRow();
  // The stack in used here contains all calls for within the subquery.
  // Hence any inbound subquery needs to be counted on its level

  // TODO:MCHACKI
  // I think we do not need to count here
  // countShadowRowProduced(stack, shadowRow.getDepth());

  if (state == ExecutorState::DONE) {
    // We have consumed everything, we are
    // Done with this query
    return ExecState::DONE;
  } else if (_lastRange.hasDataRow()) {
    // Multiple concatenated Subqueries
    return ExecState::NEXTSUBQUERY;
  } else if (_lastRange.hasShadowRow()) {
    // We still have shadowRows, we
    // need to forward them
    return ExecState::SHADOWROWS;
  } else if (_outputItemRow->isFull()) {
    // Fullfilled the call
    // Need to return!
    return ExecState::DONE;
  } else {
    // End of input, we are done for now
    // Need to call again
    return ExecState::NEXTSUBQUERY;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::sideEffectShadowRowForwarding(AqlCallStack& stack,
                                                                 SkipResult& skipResult)
    -> ExecState {
  TRI_ASSERT(executorHasSideEffects<Executor>);
  if (!stack.needToCountSubquery()) {
    // We need to really produce things here
    // fall back to original version as any other executor.
    auto res = shadowRowForwarding();
    return res;
  }
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());
  if (!_lastRange.hasShadowRow()) {
    // We got back without a ShadowRow in the LastRange
    // Let client call again
    return ExecState::DONE;
  }

  auto&& [state, shadowRow] = _lastRange.nextShadowRow();
  TRI_ASSERT(shadowRow.isInitialized());
  uint64_t depthSkippingNow = static_cast<uint64_t>(stack.shadowRowDepthToSkip());
  uint64_t shadowDepth = shadowRow.getDepth();
  bool didWriteRow = false;
  if (shadowRow.isRelevant()) {
    LOG_QUERY("1b257", DEBUG) << printTypeInfo() << " init executor.";
    // We found a relevant shadow Row.
    // We need to reset the Executor
    resetExecutor();
  }

  if (depthSkippingNow > shadowDepth) {
    // We are skipping the outermost Subquery.
    // Simply drop this ShadowRow
  } else if (depthSkippingNow == shadowDepth) {
    // We are skipping on this subquery level.
    // Skip the row, but report skipped 1.
    AqlCall& shadowCall = stack.modifyCallAtDepth(shadowDepth);
    if (shadowCall.needSkipMore()) {
      shadowCall.didSkip(1);
      shadowCall.resetSkipCount();
      skipResult.didSkipSubquery(1, shadowDepth);
    } else if (shadowCall.getLimit() > 0) {
      TRI_ASSERT(!shadowCall.needSkipMore() && shadowCall.getLimit() > 0);
      _outputItemRow->moveRow(shadowRow);
      shadowCall.didProduce(1);
      TRI_ASSERT(_outputItemRow->produced());
      _outputItemRow->advanceRow();
      didWriteRow = true;
    } else {
      TRI_ASSERT(shadowCall.hardLimit == 0);
      // Simply drop this shadowRow!
    }
  } else {
    // We got a shadowRow of a subquery we are not skipping here.
    // Do proper reporting on it's call.
    AqlCall& shadowCall = stack.modifyCallAtDepth(shadowDepth);
    TRI_ASSERT(!shadowCall.needSkipMore() && shadowCall.getLimit() > 0);
    _outputItemRow->moveRow(shadowRow);
    shadowCall.didProduce(1);

    TRI_ASSERT(_outputItemRow->produced());
    _outputItemRow->advanceRow();
    didWriteRow = true;
  }
  if (state == ExecutorState::DONE) {
    // We have consumed everything, we are
    // Done with this query
    return ExecState::DONE;
  } else if (_lastRange.hasDataRow()) {
    // Multiple concatenated Subqueries
    return ExecState::NEXTSUBQUERY;
  } else if (_lastRange.hasShadowRow()) {
    // We still have shadowRows, we
    // need to forward them
    return ExecState::SHADOWROWS;
  } else if (didWriteRow) {
    // End of input, we are done for now
    // Need to call again
    return ExecState::DONE;
  } else {
    // Done with this subquery.
    // We did not write any output yet.
    // So we can continue with upstream.
    return ExecState::UPSTREAM;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::shadowRowForwarding() -> ExecState {
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());
  if (!_lastRange.hasShadowRow()) {
    // We got back without a ShadowRow in the LastRange
    // Let us continue with the next Subquery
    return ExecState::NEXTSUBQUERY;
  }

  auto&& [state, shadowRow] = _lastRange.nextShadowRow();
  TRI_ASSERT(shadowRow.isInitialized());

  // TODO FIXME WARNING THIS IS AN UGLY HACK. PLEASE SOLVE ME IN A MORE
  // SENSIBLE WAY!
  //
  // the row fetcher doesn't know its ranges, the ranges don't know the fetcher
  //
  // ranges synchronize shadow rows, and fetcher synchronizes skipping
  //
  // but there are interactions between the two.
  if constexpr (std::is_same_v<DataRange, MultiAqlItemBlockInputRange>) {
    _rowFetcher.resetDidReturnSubquerySkips(shadowRow.getDepth());
  }

  // TODO:MCHACKI
  // I think we do not need to count here
  // countShadowRowProduced(stack, shadowRow.getDepth());
  if (shadowRow.isRelevant()) {
    LOG_QUERY("6d337", DEBUG) << printTypeInfo() << " init executor.";
    // We found a relevant shadow Row.
    // We need to reset the Executor
    resetExecutor();
  }

  _outputItemRow->moveRow(shadowRow);
  TRI_ASSERT(_outputItemRow->produced());
  _outputItemRow->advanceRow();
  if (state == ExecutorState::DONE) {
    // We have consumed everything, we are
    // Done with this query
    return ExecState::DONE;
  } else if (_lastRange.hasDataRow()) {
    // Multiple concatenated Subqueries
    return ExecState::NEXTSUBQUERY;
  } else if (_lastRange.hasShadowRow()) {
    // We still have shadowRows.
    auto const& lookAheadRow = _lastRange.peekShadowRow();
    if (lookAheadRow.isRelevant()) {
      // We are starting the NextSubquery here.
      if constexpr (Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable) {
        // TODO: Check if this works with skip forwarding
        return ExecState::SHADOWROWS;
      }
      return ExecState::NEXTSUBQUERY;
    }
    // we need to forward them
    return ExecState::SHADOWROWS;
  } else {
    // End of input, need to fetch new!
    // Just start with the next subquery.
    // If in doubt the next row will be a shadowRow again,
    // this will be forwarded than.
    return ExecState::NEXTSUBQUERY;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executeFastForward(typename Fetcher::DataRange& inputRange,
                                                      AqlCall& clientCall)
    -> std::tuple<ExecutorState, typename Executor::Stats, size_t, AqlCallType> {
  auto type = fastForwardType(clientCall, _executor);
  switch (type) {
    case FastForwardVariant::FULLCOUNT: {
      LOG_QUERY("cb135", DEBUG) << printTypeInfo() << " apply full count.";
      auto [state, stats, skippedLocal, call] = executeSkipRowsRange(_lastRange, clientCall);

      if constexpr (is_one_of_v<DataRange, AqlItemBlockInputMatrix, MultiAqlItemBlockInputRange>) {
        // The executor will have used all Rows.
        // However we need to drop them from the input
        // here.
        inputRange.skipAllRemainingDataRows();
      }

      return {state, stats, skippedLocal, call};
    }
    case FastForwardVariant::EXECUTOR: {
      LOG_QUERY("2890e", DEBUG) << printTypeInfo() << " fast forward.";
      // We use a DUMMY Call to simulate fullCount.
      AqlCall dummy;
      dummy.hardLimit = 0u;
      dummy.fullCount = true;
      auto [state, stats, skippedLocal, call] = executeSkipRowsRange(_lastRange, dummy);

      if constexpr (is_one_of_v<DataRange, AqlItemBlockInputMatrix, MultiAqlItemBlockInputRange>) {
        // The executor will have used all Rows.
        // However we need to drop them from the input
        // here.
        inputRange.skipAllRemainingDataRows();
      }

      return {state, stats, 0, call};
    }
    case FastForwardVariant::FETCHER: {
      LOG_QUERY("fa327", DEBUG) << printTypeInfo() << " bypass unused rows.";
      ADB_IGNORE_UNUSED auto const dependency = inputRange.skipAllRemainingDataRows();
      auto constexpr fastForwardCall = AqlCall{0, false, 0, AqlCall::LimitType::HARD};
      auto const call = std::invoke([&]() -> AqlCallType {
        if constexpr (std::is_same_v<AqlCallType, AqlCall>) {
          return fastForwardCall;
        } else {
#ifndef _WIN32
          // For some reason our Windows compiler complains about this static
          // assert in the cases that should be in the above constexpr path.
          // So simply not compile it in.
          static_assert(std::is_same_v<AqlCallType, AqlCallSet>);
#endif
          auto call = AqlCallSet{};
          call.calls.emplace_back(
              typename AqlCallSet::DepCallPair{dependency, AqlCallList{fastForwardCall}});
          return call;
        }
      });

      // TODO We have to ask all dependencies to go forward to the next shadow row
      auto const state = std::invoke(
          [&](auto) {
            if constexpr (std::is_same_v<DataRange, MultiAqlItemBlockInputRange>) {
              return inputRange.upstreamState(dependency);
            } else {
              return inputRange.upstreamState();
            }
          },
          0);

      return {state, typename Executor::Stats{}, 0, call};
    }
  }
  // Unreachable
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
}

/**
 * @brief This is the central function of an executor, and it acts like a
 * coroutine: It can be called multiple times and keeps state across
 * calls.
 *
 * The intended behaviour of this function is best described in terms of
 * a state machine; the possible states are the ExecStates
 * SKIP, PRODUCE, FASTFORWARD, UPSTREAM, SHADOWROWS, DONE
 *
 * SKIP       skipping rows. How rows are skipped is determined by
 *            the Executor that is used. See SkipVariants
 * PRODUCE    calls produceRows of the executor
 * FASTFORWARD again skipping rows, will count skipped rows, if fullCount is requested.
 * UPSTREAM   fetches rows from the upstream executor(s) to be processed by
 *            our executor.
 * SHADOWROWS process any shadow rows
 * DONE       processing of one output is done. We did handle offset / limit / fullCount without crossing BatchSize limits.
 *            This state does not indicate that we are DONE with all input, we are just done with one walk through this statemachine.
 *
 * We progress within the states in the following way:
 *   There is a nextState method that determines the next state based on the call, it can only lead to:
 *   SKIP, PRODUCE, FASTFORWAD, DONE
 *
 *   On the first call we will use nextState to get to our starting point.
 *   After any of SKIP, PRODUCE,, FASTFORWAD, DONE We either go to
 *   1. FASTFORWARD (if executor is done)
 *   2. DONE (if output is full)
 *   3. UPSTREAM if executor has More, (Invariant: input fully consumed)
 *   4. NextState (if none of the above applies)
 *
 *   From SHADOWROWS we can only go to DONE
 *   From UPSTREAM we go to NextState.
 *
 * @tparam Executor The Executor that will implement the logic of what needs to happen to the data
 * @param stack The call stack of lower levels
 * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>
 *        ExecutionState: WAITING -> We wait for IO, secure state, return you will be called again
 *        ExecutionState: HASMORE -> We still have data
 *        ExecutionState: DONE -> We do not have any more data, do never call again
 *        size_t -> Amount of documents skipped within this one call. (contains offset and fullCount)
 *        SharedAqlItemBlockPtr -> The resulting data
 */
template <class Executor>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
ExecutionBlockImpl<Executor>::executeWithoutTrace(AqlCallStack const& stack, AqlCallList clientCallList) {
  // We can only work on a Stack that has valid calls for all levels.
  TRI_ASSERT(stack.hasAllValidCalls());

  std::optional<AqlCallStack> stackCopy = std::nullopt;
  std::optional<AqlCallList> subqueryUpstreamCall = std::nullopt;
  // These special cases are the only ones that require to know some details of the outer stack.
  if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
    stackCopy = stack;
    subqueryUpstreamCall = stackCopy.value().popCall();
  } else if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
    stackCopy = stack;
    // TODO:MCHACKI
    // Unsure if we ca do it here, or need to do it before upstream.
    // If so, move the belove movable block up again
    stackCopy.value().pushCall(clientCallList);
  } else if constexpr (executorHasSideEffects<Executor>) {
    // If the executor has side effects, we cannot bypass any subqueries
    // by skipping them. So we need to fetch all shadow rows in order to
    // trigger this Executor with everthing from above.
    // NOTE: The Executor needs to discard shadowRows, and do the accouting.
    stackCopy = stack.createEquivalentFetchAllShadowRowsStack();
  } 

  // TODO:MCHACKI Potentially need to move this back above the std::optional values
  AqlCall clientCall = clientCallList.popNextCall();
  // We got called with a skip count already set!
  // Caller is wrong fix it.
  TRI_ASSERT(clientCall.getSkipCount() == 0);

  ExecutorState localExecutorState = ExecutorState::DONE;

  TRI_ASSERT(!(clientCall.getOffset() == 0 && clientCall.softLimit == AqlCall::Limit{0u}));
  TRI_ASSERT(!(clientCall.hasSoftLimit() && clientCall.fullCount));
  TRI_ASSERT(!(clientCall.hasSoftLimit() && clientCall.hasHardLimit()));
  // TODO:MCHACKI END OF MOVEABLE BLOCK

  if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
    // The old subquery executor can in-fact return waiting on produce call.
    // if it needs to wait for the subquery.
    // So we need to allow the return state here as well.
    TRI_ASSERT(_execState == ExecState::CHECKCALL || _execState == ExecState::SHADOWROWS ||
               _execState == ExecState::UPSTREAM || _execState == ExecState::PRODUCE ||
               _execState == ExecState::SKIP || _execState == ExecState::FASTFORWARD);
  } else {
    // We can only have returned the following internal states
    TRI_ASSERT(_execState == ExecState::CHECKCALL || _execState == ExecState::SHADOWROWS ||
               _execState == ExecState::UPSTREAM);

    // Skip can only be > 0 if we are in upstream cases, or if we got injected a block
    TRI_ASSERT(_skipped.nothingSkipped() || _execState == ExecState::UPSTREAM ||
               (std::is_same_v<Executor, IdExecutor<ConstFetcher>>));
  }

  // In some executors we may write something into the output, but then return
  // waiting. In this case we are not allowed to lose the call we have been
  // working on, we have noted down created or skipped rows in there. The client
  // is disallowed to change her mind anyways so we simply continue to work on
  // the call we already have The guarantee is, if we have returned the block,
  // and modified our local call, then the outputItemRow is not initialized
  if constexpr (!std::is_same_v<Executor, SubqueryEndExecutor>) {
    // The subqueryEndeExecutor has handled it above
    if (_outputItemRow != nullptr && _outputItemRow->isInitialized()) {
      clientCall = _outputItemRow->getClientCall();
    }
  }

  if constexpr (executorHasSideEffects<Executor>) {
    if (!_skipped.nothingSkipped()) {
      // We get woken up on upstream, but we have not reported our
      // local skip value to downstream
      // In the sideEffect executor we need to apply the skip values on the
      // incomming stack, which has not been modified yet.
      // NOTE: We only apply the skipping on subquery level.
      auto& modStack = stackCopy.value();
      TRI_ASSERT(_skipped.subqueryDepth() == modStack.subqueryLevel() + 1);
      for (size_t i = 0; i < stack.subqueryLevel(); ++i) {
        // _skipped and stack are off by one, so we need to add 1 to access
        // to _skipped.
        // They are off by one, because the callstack does not contain the
        // call for the current subquery level (what we are working on right now)
        // as this is replaced by whatever the executor would like to
        // ask from upstream.
        // The skip result is complete, and contains all subquery levels + current level.
        auto skippedSub = _skipped.getSkipOnSubqueryLevel(i + 1);
        if (skippedSub > 0) {
          auto& call = modStack.modifyCallAtDepth(i);
          call.didSkip(skippedSub);
          call.resetSkipCount();
        }
      }
    }
  }

  if (_execState == ExecState::UPSTREAM) {
    // We have been in waiting state.
    // We may have local work on the original call.
    // The client does not have the right to change her
    // mind just because we told her to hold the line.

    // The client cannot request less data!
    TRI_ASSERT(_clientRequest.requestLessDataThan(clientCall));
    clientCall = _clientRequest;

// TODO:MCHACKI don't we need this no more?
//    TRI_ASSERT(_stackBeforeWaiting.requestLessDataThan(stack));
//    stack = _stackBeforeWaiting;
  }

  auto returnToState = ExecState::CHECKCALL;

  LOG_QUERY("007ac", DEBUG) << "starting statemachine of executor " << printBlockInfo();
  while (_execState != ExecState::DONE) {
    // We can never keep state in the skipCounter
    TRI_ASSERT(clientCall.getSkipCount() == 0);
    switch (_execState) {
      case ExecState::CHECKCALL: {
        LOG_QUERY("cfe46", DEBUG)
            << printTypeInfo() << " determine next action on call " << clientCall;
        if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          // For the SubqueryEnd, we always do a fetchAll on the Executor itself.
          _execState = handleCheckCallState(stack, AqlCall{});
        } else {
          _execState = handleCheckCallState(stack, clientCall);
        }

        TRI_ASSERT(_execState == ExecState::FASTFORWARD || _execState == ExecState::SKIP ||
                   _execState == ExecState::PRODUCE || _execState == ExecState::DONE);
        break;
      }
      case ExecState::SKIP: {
        LOG_QUERY("1f786", DEBUG) << printTypeInfo() << " call skipRows " << clientCall;
        if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          TRI_ASSERT(false);
          // Subquery end will never skip the rows inside.
          // It can only be skipped outside.
        }
        if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>>) {
          // Special handling for non-spliced subqueries.
          // NOTE: The subquery Executor will by itself call EXECUTE on it's
          // subquery. This can return waiting => we can get a WAITING state
          // here. We can only get the waiting state for Subquery executors.
          ExecutionState subqueryState = ExecutionState::HASMORE;
          std::tie(_execState, subqueryState) = handleSkipStateSubquery(clientCall);
          // Translate ExecutionState => ExecutorState
          // Ordered by lieklyhood.
          switch (subqueryState) {
            case ExecutionState::HASMORE: {
              localExecutorState = ExecutorState::HASMORE;
              break;
            }
            case ExecutionState::DONE: {
              localExecutorState = ExecutorState::DONE;
              break;
            }
            case ExecutionState::WAITING: {
              TRI_ASSERT(_execState == ExecState::SKIP);
              // Need to return here, will wake up again in this state
              return {subqueryState, SkipResult{}, nullptr};
            }
          }
        } else {
          std::tie(_execState, localExecutorState) = handleSkipState(clientCall);
        }

        TRI_ASSERT(_execState == ExecState::FASTFORWARD || _execState == ExecState::UPSTREAM ||
                   _execState == ExecState::CHECKCALL);
        break;
      }
      case ExecState::PRODUCE: {
        LOG_QUERY("1f786", DEBUG) << printTypeInfo() << " call produceRows " << clientCall;
        // Make sure there's a block allocated and set
        // the call
        TRI_ASSERT(clientCall.getLimit() > 0);
        TRI_ASSERT(clientCall.getSkipCount() == 0);

        if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
          // Special handling for non-spliced subqueries.
          // NOTE: The subquery Executor will by itself call EXECUTE on it's
          // subquery. This can return waiting => we can get a WAITING state
          // here. We can only get the waiting state for Subquery executors.
          ExecutionState subqueryState = ExecutionState::HASMORE;
          std::tie(_execState, subqueryState) = handleProduceStateSubquery(clientCall);
          // Translate ExecutionState => ExecutorState
          // Ordered by lieklyhood.
          switch (subqueryState) {
            case ExecutionState::HASMORE: {
              localExecutorState = ExecutorState::HASMORE;
              break;
            }
            case ExecutionState::DONE: {
              localExecutorState = ExecutorState::DONE;
              break;
            }
            case ExecutionState::WAITING: {
              TRI_ASSERT(_execState == ExecState::SKIP);
              // Need to return here, will wake up again in this state
              return {subqueryState, SkipResult{}, nullptr};
            }
          }
        } else if constexpr (is_one_of_v<Executor, SubqueryEndExecutor>) {
          // SubqueryEnd executor always operates on a fetchAll call for the executor.
          // ClientCall is handled in ShadowRowState
          AqlCall fetchAllCall{};
          std::tie(_execState, localExecutorState) = handleProduceState(fetchAllCall);
        } else {
          std::tie(_execState, localExecutorState) = handleProduceState(clientCall);
        }
        TRI_ASSERT(_execState == ExecState::FASTFORWARD || _execState == ExecState::UPSTREAM ||
                   _execState == ExecState::CHECKCALL || _execState == ExecState::DONE);
        _executorReturnedDone = localExecutorState == ExecutorState::DONE;
        break;
      }
      case ExecState::FASTFORWARD: {
        LOG_QUERY("96e2c", DEBUG)
            << printTypeInfo() << " all produced, fast forward to end up (sub-)query.";

        if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>>) {
          // Special handling for non-spliced subqueries.
          // NOTE: The subquery Executor will by itself call EXECUTE on it's
          // subquery. This can return waiting => we can get a WAITING state
          // here. We can only get the waiting state for Subquery executors.
          ExecutionState subqueryState = ExecutionState::HASMORE;

          // Hand in a FullCount call, this will do a counting in the subquery.
          // But more importantly this will force the rows to be actively skipped.
          // This is important for modifications to get called.
          AqlCall fastForwardCall{0, true, 0, AqlCall::LimitType::HARD};
          std::tie(_execState, subqueryState) =
              handleFastForwardStateSubquery(fastForwardCall);
          // Translate ExecutionState => ExecutorState
          // Ordered by likelyhood.
          switch (subqueryState) {
            case ExecutionState::HASMORE: {
              localExecutorState = ExecutorState::HASMORE;
              break;
            }
            case ExecutionState::DONE: {
              localExecutorState = ExecutorState::DONE;
              break;
            }
            case ExecutionState::WAITING: {
              TRI_ASSERT(_execState == ExecState::SKIP);
              // Need to return here, will wake up again in this state
              return {subqueryState, SkipResult{}, nullptr};
            }
          }
        } else if constexpr (executorHasSideEffects<Executor>) {
          if (stack.needToSkipSubquery()) {
            // We are skipping an outer subquery.
            // Hand in a Fastforward call, and thereby disable any counting on the client call
            AqlCall fastForwardCall{0, false, 0, AqlCall::LimitType::HARD};
            std::tie(_execState, localExecutorState) = handleFastForwardState(fastForwardCall);
          } else {
            std::tie(_execState, localExecutorState) = handleFastForwardState(clientCall);
          }
        } else {
          std::tie(_execState, localExecutorState) = handleFastForwardState(clientCall);
        }

        TRI_ASSERT(_execState == ExecState::SHADOWROWS || _execState == ExecState::UPSTREAM ||
                   _execState == ExecState::DONE);
        break;
      }
      case ExecState::UPSTREAM: {
        LOG_QUERY("488de", DEBUG)
            << printTypeInfo() << " request dependency " << _upstreamRequest;
        // If this triggers the executors produceRows function has returned
        // HASMORE even if it knew that upstream has no further rows.
        TRI_ASSERT(_upstreamState != ExecutionState::DONE);
        if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
          TRI_ASSERT(stackCopy.has_value());
          TRI_ASSERT(subqueryUpstreamCall.has_value());
          std::tie(_execState, _upstreamState) =
              handleUpstreamState(stackCopy.value(),
                                  subqueryUpstreamCall.value().modifyNextCall(),
                                  subqueryUpstreamCall.value().peekNextCall(),
                                  subqueryUpstreamCall.value().hasMoreCalls());
        } else if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          TRI_ASSERT(stackCopy.has_value());
          AqlCall fetchAllCall{};
          std::tie(_execState, _upstreamState) =
              handleUpstreamState(stackCopy.value(), fetchAllCall, _upstreamRequest,
                                  clientCallList.hasMoreCalls());
        } else if constexpr (executorHasSideEffects<Executor>) {
          TRI_ASSERT(stackCopy.has_value());
          std::tie(_execState, _upstreamState) =
              handleUpstreamState(stackCopy.value(), clientCall, _upstreamRequest,
                                  clientCallList.hasMoreCalls());
        } else {
          std::tie(_execState, _upstreamState) =
              handleUpstreamState(stack, clientCall, _upstreamRequest,
                                  clientCallList.hasMoreCalls());
        }
        if (_upstreamState == ExecutionState::WAITING) {
          // We need to persist the old call before we return.
          // We might have some local accounting to this call.
          _clientRequest = clientCall;
          // We might also have some local accounting in this stack.
          _stackBeforeWaiting = stack;
          // We do not return anything in WAITING state, also NOT skipped.
          return {_upstreamState, SkipResult{}, nullptr};
        }
        TRI_ASSERT(_execState == ExecState::SHADOWROWS || _execState == ExecState::CHECKCALL);
        break;
      }
      case ExecState::SHADOWROWS: {
        // We only get called with something in the input.
        TRI_ASSERT(_lastRange.hasValidRow());
        LOG_QUERY("7c63c", DEBUG)
            << printTypeInfo() << " (sub-)query completed. Move ShadowRows.";

        if (outputIsFull()) {
          // We need to be able to write data
          // But maybe the existing block is full here
          // Then we need to wake up again here.
          returnToState = ExecState::SHADOWROWS;
          _execState = ExecState::DONE;
          break;
        }
        ensureOutputBlock(std::move(clientCall), _lastRange);

        TRI_ASSERT(!_outputItemRow->allRowsUsed());
        if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
          TRI_ASSERT(stackCopy.has_value());
          TRI_ASSERT(subqueryUpstreamCall.has_value());
          _execState =
              shadowRowForwardingSubqueryStart(stackCopy.value(), subqueryUpstreamCall.value().modifyNextCall());
        } else if constexpr (executorHasSideEffects<Executor>) {
          TRI_ASSERT(stackCopy.has_value());
          _execState = sideEffectShadowRowForwarding(stackCopy.value(), _skipped);
        } else {
          // This may write one or more rows.
          _execState = shadowRowForwarding();
        }

        clientCall = _outputItemRow->getClientCall();
        break;
      }
      case ExecState::NEXTSUBQUERY: {
        // This state will continue with the next run in the current subquery.
        // For this executor the input of the next run will be injected and it can continue to work.
        LOG_QUERY("0ca35", DEBUG)
            << printTypeInfo() << " ShadowRows moved, continue with next subquery.";
        if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
          TRI_ASSERT(subqueryUpstreamCall.has_value());

          auto const& currentSubqueryCall = subqueryUpstreamCall.value().peekNextCall();
          if (currentSubqueryCall.getLimit() == 0 && currentSubqueryCall.hasSoftLimit()) {
            // SoftLimitReached.
            // We cannot continue.
            _execState = ExecState::DONE;
            break;
          }
          // Otherwise just check like the other blocks
        }
        _execState = handleNextSubqueryState(stack, clientCallList);
        TRI_ASSERT(_execState == ExecState::DONE || _execState == ExecState::CHECKCALL);

        if (_execState == ExecState::CHECKCALL) {
          TRI_ASSERT(clientCallList.hasMoreCalls());
          clientCall = clientCallList.popNextCall();
        }
        break;
      }
      default:
        // unreachable
        TRI_ASSERT(false);
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    }
    TRI_ASSERT(clientCall.getSkipCount() == 0);
  }
  LOG_QUERY("80c24", DEBUG) << printBlockInfo() << " local statemachine done. Return now.";
  // If we do not have an output, we simply return a nullptr here.

  if constexpr (Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable) {
    // We can never return less rows then what we got!
    TRI_ASSERT(_outputItemRow == nullptr || _outputItemRow->numRowsLeft() == 0);
  }

  auto outputBlock = _outputItemRow != nullptr ? _outputItemRow->stealBlock()
                                               : SharedAqlItemBlockPtr{nullptr};
  // We are locally done with our output.
  // Next time we need to check the client call again
  _execState = returnToState;
  // This is not strictly necessary here, as we shouldn't be called again
  // after DONE.
  _outputItemRow.reset();

  // We return skipped here, reset member
  SkipResult skipped = _skipped;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (!stack.is36Compatible()) { 
    TRI_ASSERT(skipped.subqueryDepth() == stack.subqueryLevel() + 1 /*we took our call*/);
  }
#endif
  _skipped.reset();
  if (localExecutorState == ExecutorState::HASMORE || _lastRange.hasDataRow() ||
      _lastRange.hasShadowRow()) {
    // We have skipped or/and return data, otherwise we cannot return HASMORE
    TRI_ASSERT(!skipped.nothingSkipped() ||
               (outputBlock != nullptr && outputBlock->numEntries() > 0));
    return {ExecutionState::HASMORE, skipped, std::move(outputBlock)};
  }
  // We must return skipped and/or data when reporting HASMORE
  TRI_ASSERT(_upstreamState != ExecutionState::HASMORE ||
             (!skipped.nothingSkipped() ||
              (outputBlock != nullptr && outputBlock->numEntries() > 0)));
  return {_upstreamState, skipped, std::move(outputBlock)};
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleCheckCallState(AqlCallStack const& stack,
AqlCall const& clientCall) const -> ExecState {
  if constexpr (executorHasSideEffects<Executor>) {
    // If the executor has sideEffects, and we need to skip the results we would
    // produce here because we actually skip the subquery, we instead do a
    // hardLimit 0 (aka FastForward) call instead to the local Executor
    if (stack.needToSkipSubquery()) {
      return ExecState::FASTFORWARD;
    }
  }
  return nextState(clientCall);
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleSkipState(arangodb::aql::AqlCall& clientCall) -> std::pair<ExecState, ExecutorState> {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto const offsetBefore = clientCall.getOffset();
  TRI_ASSERT(offsetBefore > 0);
  bool const canPassFullcount = clientCall.getLimit() == 0 && clientCall.needsFullCount();
#endif
  ExecutorState state = ExecutorState::HASMORE;
  typename Executor::Stats stats;
  size_t skippedLocal = 0;
  AqlCallType call{};
  // Execute skipSome
  std::tie(state, stats, skippedLocal, call) =
      executeSkipRowsRange(_lastRange, clientCall);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // Assertion: We did skip 'skippedLocal' documents here.
  // This means that they have to be removed from
  // clientCall.getOffset() This has to be done by the Executor
  // calling call.didSkip() accordingly. The LIMIT executor with a
  // LIMIT of 0 can also bypass fullCount here, even if callLimit > 0
  if (canPassFullcount || std::is_same_v<Executor, LimitExecutor>) {
    // In this case we can first skip. But straight after continue with fullCount, so we might skip more
    TRI_ASSERT(clientCall.getOffset() + skippedLocal >= offsetBefore);
    if (clientCall.getOffset() + skippedLocal > offsetBefore) {
      // First need to count down offset.
      TRI_ASSERT(clientCall.getOffset() == 0);
    }
  } else {
    TRI_ASSERT(clientCall.getOffset() + skippedLocal == offsetBefore);
  }
#endif
  _skipped.didSkip(skippedLocal);
  _blockStats += stats;
  // The execute might have modified the client call.
  if (state == ExecutorState::DONE) {
    return {ExecState::FASTFORWARD, state};
  } else if (clientCall.getOffset() > 0) {
    TRI_ASSERT(_upstreamState != ExecutionState::DONE);
    // We need to request more
    _upstreamRequest = call;
    return {ExecState::UPSTREAM, state};
  }
  // We are done with skipping. Skip is not allowed to request more
  return {ExecState::CHECKCALL, state};
}

template <>
template <>
auto ExecutionBlockImpl<SubqueryExecutor<true>>::handleSkipStateSubquery(arangodb::aql::AqlCall& clientCall) -> std::pair<ExecState, ExecutionState> {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto const offsetBefore = clientCall.getOffset();
  TRI_ASSERT(offsetBefore > 0);
  bool const canPassFullcount = clientCall.getLimit() == 0 && clientCall.needsFullCount();
#endif
  ExecutionState state = ExecutionState::HASMORE;
  typename SubqueryExecutor<true>::Stats stats;
  size_t skippedLocal = 0;
  AqlCallType call{};
  {
    TRI_DEFER(clientCall.resetSkipCount());

    std::tie(state, stats, skippedLocal, call) =
        _executor.skipRowsRange(_lastRange, clientCall);
    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(skippedLocal == 0);
      return {ExecState::SKIP, state};
    }
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // Assertion: We did skip 'skippedLocal' documents here.
  // This means that they have to be removed from
  // clientCall.getOffset() This has to be done by the Executor
  // calling call.didSkip() accordingly. The LIMIT executor with a
  // LIMIT of 0 can also bypass fullCount here, even if callLimit > 0
  if (canPassFullcount) {
    // In this case we can first skip. But straight after continue with fullCount, so we might skip more
    TRI_ASSERT(clientCall.getOffset() + skippedLocal >= offsetBefore);
    if (clientCall.getOffset() + skippedLocal > offsetBefore) {
      // First need to count down offset.
      TRI_ASSERT(clientCall.getOffset() == 0);
    }
  } else {
    TRI_ASSERT(clientCall.getOffset() + skippedLocal == offsetBefore);
  }
#endif
  _skipped.didSkip(skippedLocal);
  _blockStats += stats;
  // The execute might have modified the client call.
  if (state == ExecutionState::DONE) {
    return {ExecState::FASTFORWARD, state};
  } else if (clientCall.getOffset() > 0) {
    TRI_ASSERT(_upstreamState != ExecutionState::DONE);
    // We need to request more
    _upstreamRequest = call;
    return {ExecState::UPSTREAM, state};
  }
  // We are done with skipping. Skip is not allowed to request more
  return {ExecState::CHECKCALL, state};
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleProduceState(AqlCall& clientCall)
    -> std::pair<ExecState, ExecutorState> {
  if constexpr (is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
    // This is forbidden no Subquery can be used here.
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
  } else {
    if (outputIsFull()) {
      // We need to be able to write data
      // But maybe the existing block is full here
      // Then we need to wake up again.
      // However the client might decide on a different
      // call, so we do not record this position
      return {ExecState::DONE, ExecutorState::HASMORE};
    }

    ensureOutputBlock(std::move(clientCall), _lastRange);

    TRI_ASSERT(_outputItemRow);
    TRI_ASSERT(!_executorReturnedDone);
    ExecutorState state = ExecutorState::HASMORE;
    typename Executor::Stats stats;
    auto call = AqlCallType{};
    // Execute produceRows
    std::tie(state, stats, call) = executeProduceRows(_lastRange, *_outputItemRow);
    
    _executorReturnedDone = state == ExecutorState::DONE;
    _blockStats += stats;


    clientCall = _outputItemRow->getClientCall();

    if (state == ExecutorState::DONE) {
      return {ExecState::FASTFORWARD, state};
    } else if ((Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable ||
                clientCall.getLimit() > 0) &&
               outputIsFull()) {
      // In pass through variant we need to stop whenever the block is full.
      // In all other branches only if the client Still needs more data.
      return {ExecState::DONE, state};
    } else if (clientCall.getLimit() > 0 && executorNeedsCall(call)) {
      TRI_ASSERT(_upstreamState != ExecutionState::DONE);
      // We need to request more
      _upstreamRequest = call;
      return {ExecState::UPSTREAM, state};
    } else {
      // We are done with producing. Produce is not allowed to request more
      return {ExecState::CHECKCALL, state};
    }
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleProduceStateSubquery(AqlCall& clientCall)
    -> std::pair<ExecState, ExecutionState> {
  if constexpr (!is_one_of_v<Executor, SubqueryExecutor<true>, SubqueryExecutor<false>>) {
    // This is forbidden only Subquery can be used here.
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
  } else {
    if (outputIsFull()) {
      // We need to be able to write data
      // But maybe the existing block is full here
      // Then we need to wake up again.
      // However the client might decide on a different
      // call, so we do not record this position
      return {ExecState::DONE, ExecutionState::HASMORE};
    }
    ensureOutputBlock(std::move(clientCall), _lastRange);

    TRI_ASSERT(_outputItemRow);
    TRI_ASSERT(!_executorReturnedDone);
    ExecutionState state = ExecutionState::HASMORE;
    typename Executor::Stats stats;
    auto call = AqlCallType{};
    std::tie(state, stats, call) = _executor.produceRows(_lastRange, *_outputItemRow);
    if (state == ExecutionState::WAITING) {
      return {ExecState::SKIP, state};
    }

    _blockStats += stats;

    clientCall = _outputItemRow->getClientCall();

    if (state == ExecutionState::DONE) {
      return {ExecState::FASTFORWARD, state};
    } else if ((Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable ||
                clientCall.getLimit() > 0) &&
               outputIsFull()) {
      // In pass through variant we need to stop whenever the block is full.
      // In all other branches only if the client Still needs more data.
      return {ExecState::DONE, state};
    } else if (clientCall.getLimit() > 0 && executorNeedsCall(call)) {
      TRI_ASSERT(_upstreamState != ExecutionState::DONE);
      // We need to request more
      _upstreamRequest = call;
      return {ExecState::UPSTREAM, state};
    } else {
      // We are done with producing. Produce is not allowed to request more
      return {ExecState::CHECKCALL, state};
    }
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleFastForwardState(AqlCall& clientCall)
    -> std::pair<ExecState, ExecutorState> {
  TRI_DEFER(clientCall.resetSkipCount());

  ExecutorState state = ExecutorState::HASMORE;
  typename Executor::Stats stats;
  size_t skippedLocal = 0;
  AqlCallType call{};

  // Execute skipSome
  std::tie(state, stats, skippedLocal, call) = executeFastForward(_lastRange, clientCall);

  _skipped.didSkip(skippedLocal);
  _blockStats += stats;

  if (state == ExecutorState::DONE) {
    if (!_lastRange.hasValidRow()) {
      return {ExecState::DONE, state};
    } else {
      return {ExecState::SHADOWROWS, state};
    }
  }
  // We need to request more
  _upstreamRequest = call;
  return {ExecState::UPSTREAM, state};
}

template <>
template <>
auto ExecutionBlockImpl<SubqueryExecutor<true>>::handleFastForwardStateSubquery(AqlCall& clientCall)
    -> std::pair<ExecState, ExecutionState> {
  TRI_DEFER(clientCall.resetSkipCount());

  typename SubqueryExecutor<true>::Stats stats;
  size_t skippedLocal = 0; // unused
  AqlCallType call{};

  // NOTE: The subquery Executor will by itself call EXECUTE on it's
  // subquery. This can return waiting => we can get a WAITING state
  // here. We can only get the waiting state for Subquery executors.
  ExecutionState state = ExecutionState::HASMORE;

  std::tie(state, stats, skippedLocal, call) = _executor.skipRowsRange(_lastRange, clientCall);
  if (state == ExecutionState::WAITING) {
    TRI_ASSERT(skippedLocal == 0);
    return {ExecState::SKIP, state};
  }

  _blockStats += stats;

  if (state == ExecutionState::DONE) {
    if (ADB_LIKELY(!_lastRange.hasValidRow())) {
      // For now we can either have shadowRows or SubqueryExecutors, due to how the rule works.
      // Not both, but nothing stops us to allow a mix here.
      return {ExecState::DONE, state};
    } else {
      return {ExecState::SHADOWROWS, state};
    }
  }
  // We need to request more
  _upstreamRequest = call;
  return {ExecState::UPSTREAM, state};
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleUpstreamState(AqlCallStack const& stack,
                                                       AqlCall& clientCall,
                                                       AqlCallType const& upstreamCall,
                                                       bool wasCalledWithContinueCall)
    -> std::pair<ExecState, ExecutionState> {
  // We need to make sure _lastRange is all used for single-dependency
  // executors.
  TRI_ASSERT(isMultiDepExecutor<Executor> || !lastRangeHasDataRow());
  TRI_ASSERT(!_lastRange.hasShadowRow());
  SkipResult skippedLocal;
  ExecutionState upstreamState = ExecutionState::HASMORE;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto subqueryLevelBefore = stack.subqueryLevel();
#endif
  std::tie(upstreamState, skippedLocal, _lastRange) =
      executeFetcher(stack, upstreamCall, wasCalledWithContinueCall);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(subqueryLevelBefore == stack.subqueryLevel());
#endif

  if (upstreamState == ExecutionState::WAITING) {
    return {ExecState::UPSTREAM, upstreamState};
  }

// TODO:MCHACKI skip counting needs implementation
#if 0
  if (!skippedLocal.nothingSkipped()) {
    if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
      // In SubqueryStart the stack is exactly the same size as the skip result
      // from above, the call we work on is inside the subquery.
      // The stack is exactly what we send upstream, no added call on top.
      TRI_ASSERT(skippedLocal.subqueryDepth() == stack.subqueryLevel());
      for (size_t i = 0; i < stack.subqueryLevel(); ++i) {
        auto skippedSub = skippedLocal.getSkipOnSubqueryLevel(i);
        if (skippedSub > 0) {
          auto& call = stack.modifyCallAtDepth(i);
          call.didSkip(skippedSub);
          call.resetSkipCount();
        }
      }
    } else {
      // In all other executors the stack is 1 depth smaller then what
      // we request from upstream. The top-most entry will be added
      // by the executor and is not part of the stack here.
      // However the returned skipped information is complete including
      // the local call.
      TRI_ASSERT(skippedLocal.subqueryDepth() == stack.subqueryLevel() + 1);

      for (size_t i = 0; i < stack.subqueryLevel(); ++i) {
        auto skippedSub = skippedLocal.getSkipOnSubqueryLevel(i + 1);
        if (skippedSub > 0) {
          auto& call = stack.modifyCallAtDepth(i);
          call.didSkip(skippedSub);
          call.resetSkipCount();
        }
      }
    }
  }
#endif
  if constexpr (Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable) {
    // We have a new range, passthrough can use this range.
    _hasUsedDataRangeBlock = false;
  }

  if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
    // We need to pop the last subquery from the returned skip
    // We have not asked for a subquery skip.
    TRI_ASSERT(skippedLocal.getSkipCount() == 0);
    skippedLocal.decrementSubquery();
  }

  if constexpr (skipRowsType<Executor>() == SkipRowsRangeVariant::FETCHER) {
    // We skipped through passthrough, so count that a skip was solved.
    _skipped.merge(skippedLocal, false);
    clientCall.didSkip(skippedLocal.getSkipCount());
    clientCall.resetSkipCount();
  } else if constexpr (is_one_of_v<Executor, SubqueryStartExecutor, SubqueryEndExecutor>) {
    // Subquery needs to include the topLevel Skip.
    // But does not need to apply the count to clientCall.
    _skipped.merge(skippedLocal, false);

  } else {
    _skipped.merge(skippedLocal, true);
  }
  if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
    // For the subqueryStart, we need to increment the SkipLevel by one
    // as we may trigger this multiple times, check if we need to do it.
    while (_skipped.subqueryDepth() <= skippedLocal.subqueryDepth()) {
      // In fact, we only need to increase by 1
      // the lower levels have been merged above
      TRI_ASSERT(_skipped.subqueryDepth() == skippedLocal.subqueryDepth());
      _skipped.incrementSubquery();
    }
  }

  if (_lastRange.hasShadowRow() && !_lastRange.peekShadowRow().isRelevant()) {
    return {ExecState::SHADOWROWS, upstreamState};
  }
  return {ExecState::CHECKCALL, upstreamState};
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::handleNextSubqueryState(AqlCallStack const& stack, AqlCallList const& clientCallList) -> ExecState {
  if (!stack.hasAllValidCalls()) {
    // We can only continue if we still have a valid call
    // on all levels
    return ExecState::DONE;
  }

  if (clientCallList.hasMoreCalls()) {
    // Update to next call and start all over.
    return ExecState::CHECKCALL;
  } else {
    // We cannot continue, so we are done
    return ExecState::DONE;
  }
}

template <class Executor>
void ExecutionBlockImpl<Executor>::resetExecutor() {
  // cppcheck-suppress unreadVariable
  constexpr bool customInit = hasInitializeCursor<Executor>::value;
  // IndexExecutor and EnumerateCollectionExecutor have initializeCursor
  // implemented, so assert this implementation is used.
  static_assert(!std::is_same<Executor, EnumerateCollectionExecutor>::value || customInit,
                "EnumerateCollectionExecutor is expected to implement a custom "
                "initializeCursor method!");
  static_assert(!std::is_same<Executor, IndexExecutor>::value || customInit,
                "IndexExecutor is expected to implement a custom "
                "initializeCursor method!");
  static_assert(!std::is_same<Executor, DistinctCollectExecutor>::value || customInit,
                "DistinctCollectExecutor is expected to implement a custom "
                "initializeCursor method!");
  InitializeCursor<customInit>::init(_executor, _rowFetcher, _executorInfos);
  _executorReturnedDone = false;
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::outputIsFull() const noexcept -> bool {
  return _outputItemRow != nullptr && _outputItemRow->isInitialized() &&
         _outputItemRow->allRowsUsed();
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::lastRangeHasDataRow() const noexcept -> bool {
  return _lastRange.hasDataRow();
}

template <>
template <>
RegisterId ExecutionBlockImpl<IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>::getOutputRegisterId() const
    noexcept {
  return _executorInfos.getOutputRegister();
}

template <class Executor>
void ExecutionBlockImpl<Executor>::init() {
  TRI_ASSERT(!_initialized);
  if constexpr (isMultiDepExecutor<Executor>) {
    _lastRange.resizeOnce(ExecutorState::HASMORE, 0, _dependencies.size());
    _rowFetcher.init();
  }
}

template <class Executor>
void ExecutionBlockImpl<Executor>::initOnce() {
  if (!_initialized) {
    init();
    _initialized = true;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::executorNeedsCall(AqlCallType& call) const
    noexcept -> bool {
  if constexpr (isMultiDepExecutor<Executor>) {
    // call is an AqlCallSet. We need to call upstream if it's not empty.
    return !call.empty();
  } else {
    // call is an AqlCall, unconditionally. The current convention is
    // to call upstream when there is no input left.
    // This could be made unnecessary by returning an optional AqlCall
    // for single-dependency executors.
    return !lastRangeHasDataRow();
  }
};

template <class Executor>
auto ExecutionBlockImpl<Executor>::memoizeCall(AqlCall const& call,
                                               bool wasCalledWithContinueCall) noexcept
    -> void {
  if (!_hasMemoizedCall) {
    if constexpr (!isMultiDepExecutor<Executor>) {
      // We can only try to memoize the first call ever send.
      // Otherwise the call might be influenced by state
      // inside the Executor
      if (wasCalledWithContinueCall && call.getOffset() == 0 &&
          !call.needsFullCount() && !call.hasSoftLimit()) {
        // First draft, we only memoize non-skipping calls
        _defaultUpstreamRequest = call;
      }
    }
    _hasMemoizedCall = true;
  }
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::createUpstreamCall(AqlCall const& call, bool wasCalledWithContinueCall)
    -> AqlCallList {
  // We can only memoize the first call
  memoizeCall(call, wasCalledWithContinueCall);
  TRI_ASSERT(_hasMemoizedCall);
  if constexpr (!isMultiDepExecutor<Executor>) {
    if (_defaultUpstreamRequest.has_value()) {
      // We have memoized a default call.
      // So we can use it.
      return AqlCallList{call, _defaultUpstreamRequest.value()};
    }
  }
  return AqlCallList{call};
}

template <class Executor>
auto ExecutionBlockImpl<Executor>::countShadowRowProduced(AqlCallStack& stack, size_t depth)
    -> void {
  auto& subList = stack.modifyCallListAtDepth(depth);
  auto& subCall = subList.modifyNextCall();
  subCall.didProduce(1);
  if (depth > 0) {
    // We have written a ShadowRow.
    // Pop the corresponding production call.
    std::ignore = stack.modifyCallListAtDepth(depth - 1).popNextCall();
  }
}

template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::Condition>>;
template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::Reference>>;
template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::V8Condition>>;
template class ::arangodb::aql::ExecutionBlockImpl<ConstrainedSortExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<CountCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<DistinctCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<EnumerateCollectionExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<EnumerateListExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<FilterExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<HashedCollectExecutor>;

template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::Materialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<
    IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<
    IResearchViewExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::Materialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<
    IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<
    IResearchViewExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::NotMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::LateMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<false, arangodb::iresearch::MaterializeType::Materialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<
    false, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<
    false, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::NotMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::LateMaterialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<true, arangodb::iresearch::MaterializeType::Materialize>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<
    true, arangodb::iresearch::MaterializeType::NotMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewMergeExecutor<
    true, arangodb::iresearch::MaterializeType::LateMaterialize | arangodb::iresearch::MaterializeType::UseStoredValues>>;

template class ::arangodb::aql::ExecutionBlockImpl<IdExecutor<ConstFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>;
template class ::arangodb::aql::ExecutionBlockImpl<IndexExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<LimitExecutor>;

// IndexTag, Insert, Remove, Update,Replace, Upsert are only tags for this one
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<IndexTag>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Insert>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Remove>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Update>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Replace>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Upsert>>;

template class ::arangodb::aql::ExecutionBlockImpl<NoResultsExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<ReturnExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<ShortestPathExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<KShortestPathsExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortedCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SubqueryEndExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SubqueryExecutor<true>>;
template class ::arangodb::aql::ExecutionBlockImpl<SubqueryExecutor<false>>;
template class ::arangodb::aql::ExecutionBlockImpl<SubqueryStartExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<TraversalExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortingGatherExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<ParallelUnsortedGatherExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<UnsortedGatherExecutor>;

template class ::arangodb::aql::ExecutionBlockImpl<MaterializeExecutor<RegisterId>>;
template class ::arangodb::aql::ExecutionBlockImpl<MaterializeExecutor<std::string const&>>;

template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, InsertModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, InsertModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, RemoveModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, RemoveModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, UpdateReplaceModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpdateReplaceModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, UpsertModifier>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpsertModifier>>;
