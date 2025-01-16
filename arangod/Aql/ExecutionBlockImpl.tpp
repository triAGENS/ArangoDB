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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ExecutionBlockImpl.h"

#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemBlockManager.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionState.h"
#include "Aql/Executor/IResearchViewExecutor.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/RegisterInfos.h"
#include "Aql/SimpleModifier.h"
#include "Aql/SkipResult.h"
#include "Aql/Timing.h"
#include "Aql/UpsertModifier.h"
#include "Aql/types.h"
#include "Basics/ScopeGuard.h"
#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/SingleServerProvider.h"
#include "Graph/Steps/ClusterProviderStep.h"
#include "Graph/Steps/SingleServerProviderStep.h"
#include "Graph/algorithm-aliases.h"
#include "Scheduler/SchedulerFeature.h"

#include <absl/strings/str_cat.h>

#include <type_traits>

namespace arangodb {
namespace aql {
class MultiDependencySingleRowFetcher;
class MultiAqlItemBlockInputRange;

// These tags are used to instantiate SingleRemoteModificationExecutor
// for the different use cases
struct IndexTag;
struct Insert;
struct Remove;
struct Replace;
struct Update;
struct Upsert;

template<typename Modifier>
struct SingleRemoteModificationExecutor;

class SortedCollectExecutor;
class SubqueryEndExecutor;
class SubqueryStartExecutor;

class FilterExecutor;
class HashedCollectExecutor;
class NoResultsExecutor;

class ConstFetcher;
template<class UsedFetcher>
class IdExecutor;
class IndexExecutor;
class JoinExecutor;
class MaterializeRocksDBExecutor;
class MaterializeSearchExecutor;
template<typename FetcherType, typename ModifierType>
class ModificationExecutor;
struct IndexDistinctScanExecutor;
struct IndexAggregateScanExecutor;

class LimitExecutor;
class ReturnExecutor;
class SortExecutor;
class AccuWindowExecutor;
class WindowExecutor;

class TraversalExecutor;
template<class FinderType>
class EnumeratePathsExecutor;
template<class FinderType>
class ShortestPathExecutor;

class SortingGatherExecutor;
class UnsortedGatherExecutor;
class ParallelUnsortedGatherExecutor;

enum class CalculationType;
template<CalculationType calculationType>
class CalculationExecutor;

class ConstrainedSortExecutor;
class CountCollectExecutor;
class DistinctCollectExecutor;
class EnumerateCollectionExecutor;
class EnumerateListExecutor;
class EnumerateListObjectExecutor;
class GroupedSortExecutor;
class EnumerateNearVectorsExecutor;
}  // namespace aql

namespace graph {
class KShortestPathsFinderInterface;
}

namespace iresearch {
// only available in Enterprise
class OffsetMaterializeExecutor;
}  // namespace iresearch
}  // namespace arangodb

/* SingleServerProvider Section */
using SingleServerProviderStep = ::arangodb::graph::SingleServerProviderStep;

using KPath = arangodb::graph::KPathEnumerator<
    arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using KPathTracer = arangodb::graph::TracedKPathEnumerator<
    arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using AllShortestPaths = arangodb::graph::AllShortestPathsEnumerator<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;
using AllShortestPathsTracer =
    arangodb::graph::TracedAllShortestPathsEnumerator<
        arangodb::graph::SingleServerProvider<
            arangodb::graph::SingleServerProviderStep>>;

using KShortestPaths = arangodb::graph::KShortestPathsEnumerator<
    arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using KShortestPathsTracer = arangodb::graph::TracedKShortestPathsEnumerator<
    arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using YenPaths = arangodb::graph::YenEnumeratorWithProvider<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;

using YenPathsTracer = arangodb::graph::TracedYenEnumeratorWithProvider<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;

using YenPathsCluster = arangodb::graph::YenEnumeratorWithProvider<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using YenPathsClusterTracer = arangodb::graph::TracedYenEnumeratorWithProvider<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedYenPaths = arangodb::graph::WeightedYenEnumeratorWithProvider<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;

using WeightedYenPathsTracer =
    arangodb::graph::TracedWeightedYenEnumeratorWithProvider<
        arangodb::graph::SingleServerProvider<
            arangodb::graph::SingleServerProviderStep>>;

using WeightedYenPathsCluster =
    arangodb::graph::WeightedYenEnumeratorWithProvider<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedYenPathsClusterTracer =
    arangodb::graph::TracedWeightedYenEnumeratorWithProvider<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedKShortestPaths =
    arangodb::graph::WeightedKShortestPathsEnumerator<
        arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using WeightedKShortestPathsTracer =
    arangodb::graph::TracedWeightedKShortestPathsEnumerator<
        arangodb::graph::SingleServerProvider<SingleServerProviderStep>>;

using ShortestPath = arangodb::graph::ShortestPathEnumerator<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;
using ShortestPathTracer = arangodb::graph::TracedShortestPathEnumerator<
    arangodb::graph::SingleServerProvider<
        arangodb::graph::SingleServerProviderStep>>;

using WeightedShortestPath =
    arangodb::graph::WeightedShortestPathEnumeratorAlias<
        arangodb::graph::SingleServerProvider<
            arangodb::graph::SingleServerProviderStep>>;
using WeightedShortestPathTracer =
    arangodb::graph::TracedWeightedShortestPathEnumeratorAlias<
        arangodb::graph::SingleServerProvider<
            arangodb::graph::SingleServerProviderStep>>;

/* ClusterProvider Section */
using KPathCluster = arangodb::graph::KPathEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using KPathClusterTracer = arangodb::graph::TracedKPathEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using AllShortestPathsCluster = arangodb::graph::AllShortestPathsEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;
using AllShortestPathsClusterTracer =
    arangodb::graph::TracedAllShortestPathsEnumerator<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using KShortestPathsCluster = arangodb::graph::KShortestPathsEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using KShortestPathsClusterTracer =
    arangodb::graph::TracedKShortestPathsEnumerator<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedKShortestPathsCluster =
    arangodb::graph::WeightedKShortestPathsEnumerator<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedKShortestPathsClusterTracer =
    arangodb::graph::TracedWeightedKShortestPathsEnumerator<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using ShortestPathCluster = arangodb::graph::ShortestPathEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;
using ShortestPathClusterTracer = arangodb::graph::TracedShortestPathEnumerator<
    arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

using WeightedShortestPathCluster =
    arangodb::graph::WeightedShortestPathEnumeratorAlias<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;
using WeightedShortestPathClusterTracer =
    arangodb::graph::TracedWeightedShortestPathEnumeratorAlias<
        arangodb::graph::ClusterProvider<arangodb::graph::ClusterProviderStep>>;

namespace arangodb::aql {
struct MultipleRemoteModificationExecutor;
}

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
  template<typename T>                                               \
  class checkName {                                                  \
    template<typename C>                                             \
    static std::true_type test(decltype(&C::methodName));            \
    template<typename C>                                             \
    static std::true_type test(decltype(&C::template methodName<>)); \
    template<typename>                                               \
    static std::false_type test(...);                                \
                                                                     \
   public:                                                           \
    static constexpr bool value = decltype(test<T>(0))::value;       \
  }

CREATE_HAS_MEMBER_CHECK(initializeCursor, hasInitializeCursor);
CREATE_HAS_MEMBER_CHECK(expectedNumberOfRows, hasExpectedNumberOfRows);
CREATE_HAS_MEMBER_CHECK(skipRowsRange, hasSkipRowsRange);

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

template<typename Executor>
constexpr bool executorHasSideEffects = is_one_of_v<
    Executor,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         InsertModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         RemoveModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         UpdateReplaceModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         UpsertModifier>>;

template<typename Executor>
constexpr bool executorCanReturnWaiting = is_one_of_v<
    Executor,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         InsertModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         RemoveModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         UpdateReplaceModifier>,
    ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>,
                         UpsertModifier>>;

template<class Executor>
ExecutionBlockImpl<Executor>::ExecutionBlockImpl(
    ExecutionEngine* engine, ExecutionNode const* node,
    RegisterInfos registerInfos, typename Executor::Infos executorInfos)
    : ExecutionBlock(engine, node),
      _registerInfos(std::move(registerInfos)),
      _dependencyProxy(_dependencies, _registerInfos.numberOfInputRegisters()),
      _rowFetcher(std::in_place, _dependencyProxy),
      _executorInfos(std::move(executorInfos)),
      _executor(std::in_place, *_rowFetcher, _executorInfos),
      _query(engine->getQuery()),
      _state(InternalState::FETCH_DATA),
      _execState{ExecState::CHECKCALL},
      _lastRange{MainQueryState::HASMORE},
      _upstreamRequest{},
      _clientRequest{},
      _stackBeforeWaiting{AqlCallStack::Empty{}},
      _hasUsedDataRangeBlock{false} {
  if (_exeNode->isCallstackSplitEnabled()) {
    _callstackSplit = std::make_unique<CallstackSplit>(*this);
  }
}

template<class Executor>
ExecutionBlockImpl<Executor>::~ExecutionBlockImpl() {
  if (_prefetchTask && !_prefetchTask->isConsumed() &&
      !_prefetchTask->tryClaim()) {
    // some thread is still working on our prefetch task
    // -> we need to wait for that task to finish first!
    _prefetchTask->waitFor();
  }
}

template<class Executor>
std::unique_ptr<OutputAqlItemRow> ExecutionBlockImpl<Executor>::createOutputRow(
    SharedAqlItemBlockPtr&& newBlock, AqlCall&& call) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (newBlock != nullptr) {
    // Assert that the block has enough registers. This must be guaranteed by
    // the register planning.
    TRI_ASSERT(newBlock->numRegisters() ==
               _registerInfos.numberOfOutputRegisters())
        << "newBlock->numRegisters() = " << newBlock->numRegisters()
        << " _registerInfos.numberOfOutputRegisters() = "
        << _registerInfos.numberOfOutputRegisters();
    // Check that all output registers are empty.
    size_t const n = newBlock->numRows();
    auto const& regs = _registerInfos.getOutputRegisters();
    if (!regs.empty()) {
      bool const hasShadowRows = newBlock->hasShadowRows();
      for (size_t row = 0; row < n; row++) {
        if (!hasShadowRows || !newBlock->isShadowRow(row)) {
          for (auto const& reg : regs) {
            AqlValue const& val = newBlock->getValueReference(row, reg);
            TRI_ASSERT(val.isEmpty() && reg.isRegularRegister())
                << std::boolalpha << "val.isEmpty() = " << val.isEmpty()
                << " reg.isRegularRegister() = " << reg.isRegularRegister()
                << " reg = " << reg.value()
                << " value = " << val.slice().toJson();
          }
        }
      }
    }
  }
#endif

  constexpr auto copyRowBehaviour = [] {
    if constexpr (Executor::Properties::allowsBlockPassthrough ==
                  BlockPassthrough::Enable) {
      return OutputAqlItemRow::CopyRowBehavior::DoNotCopyInputRows;
    } else {
      return OutputAqlItemRow::CopyRowBehavior::CopyInputRows;
    }
  }();

  return std::make_unique<OutputAqlItemRow>(
      std::move(newBlock), registerInfos().getOutputRegisters(),
      registerInfos().registersToKeep(), registerInfos().registersToClear(),
      std::move(call), copyRowBehaviour);
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executor() -> Executor& {
  TRI_ASSERT(_executor.has_value());
  if (!_executor.has_value()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no executor available in query");
  }
  return *_executor;
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::fetcher() noexcept -> Fetcher& {
  TRI_ASSERT(_rowFetcher.has_value());
  return *_rowFetcher;
}

template<class Executor>
QueryContext const& ExecutionBlockImpl<Executor>::getQuery() const {
  return _query;
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executorInfos() const
    -> ExecutorInfos const& {
  return _executorInfos;
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::registerInfos() const
    -> RegisterInfos const& {
  return _registerInfos;
}

template<class Executor>
std::pair<ExecutionState, Result>
ExecutionBlockImpl<Executor>::initializeCursor(InputAqlItemRow const& input) {
  // reinitialize the DependencyProxy
  _dependencyProxy.reset();
  _hasUsedDataRangeBlock = false;
  initOnce();
  // destroy and re-create the Fetcher
  _rowFetcher.emplace(_dependencyProxy);

  if constexpr (isMultiDepExecutor<Executor>) {
    _lastRange.reset();
    fetcher().init();
  } else {
    _lastRange = DataRange(MainQueryState::HASMORE);
  }

  TRI_ASSERT(_skipped.nothingSkipped());
  _skipped.reset();
  TRI_ASSERT(_state == InternalState::DONE ||
             _state == InternalState::FETCH_DATA);
  _state = InternalState::FETCH_DATA;

  if constexpr (std::is_same_v<Executor, IdExecutor<ConstFetcher>>) {
    SharedAqlItemBlockPtr block = input.cloneToBlock(
        _engine->itemBlockManager(), registerInfos().registersToKeep().back(),
        registerInfos().numberOfOutputRegisters());
    // We inject an empty copy of our skipped here,
    // This is resetted, but will maintain the size
    fetcher().injectBlock(std::move(block), _skipped);
  }
  resetExecutor();

  return ExecutionBlock::initializeCursor(input);
}

template<class Executor>
void ExecutionBlockImpl<Executor>::collectExecStats(ExecutionStats& stats) {
  // some node types provide info about how many documents have been
  // filtered. if so, use the info and add it to the node stats.
  if constexpr (is_one_of_v<typename Executor::Stats, IndexStats,
                            EnumerateCollectionStats, FilterStats,
                            TraversalStats, MaterializeStats>) {
    _execNodeStats.filtered += _blockStats.getFiltered();
  }
  ExecutionBlock::collectExecStats(stats);
  stats += _blockStats;  // additional stats;
}

template<class Executor>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
ExecutionBlockImpl<Executor>::execute(AqlCallStack const& stack) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  bool old = false;
  TRI_ASSERT(_isBlockInUse.compare_exchange_strong(old, true));
  TRI_ASSERT(_isBlockInUse);
#endif

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto guard = scopeGuard([&]() noexcept {
    bool old = true;
    TRI_ASSERT(_isBlockInUse.compare_exchange_strong(old, false));
    TRI_ASSERT(!_isBlockInUse);
  });
#endif

  if (getQuery().killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  // check if this block failed already.
  if (ADB_UNLIKELY(_firstFailure.fail())) {
    // if so, just return the stored error.
    // we need to do this because if a block fails with
    // an exception, it is in an invalid state, and all
    // subsequent calls into it may behave badly.
    THROW_ARANGO_EXCEPTION(_firstFailure);
  }

  traceExecuteBegin(stack);
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

  try {
    initOnce();
    auto res = executeWithoutTrace(stack);

    if (std::get<0>(res) != ExecutionState::WAITING) {
      // if we have finished processing our input we reset _outputItemRow and
      // _lastRange to release the SharedAqlItemBlockPtrs. This is necessary to
      // avoid concurrent refCount updates in case of async prefetching because
      // refCount is intentionally not atomic.
      _outputItemRow.reset();
      if constexpr (!isMultiDepExecutor<Executor>) {
        if (!_lastRange.hasValidRow()) {
          _lastRange.reset();
        }
      }
    }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    auto const& [state, skipped, block] = res;
    if (block != nullptr) {
      block->validateShadowRowConsistency();
    }
#endif
    traceExecuteEnd(res);
    return res;
  } catch (basics::Exception const& ex) {
    TRI_ASSERT(_firstFailure.ok());
    // store only the first failure we got
    setFailure({ex.code(),
                absl::StrCat(ex.what(), " [node #", getPlanNode()->id().id(),
                             ": ", getPlanNode()->getTypeString(), "]")});
    LOG_QUERY("7289a", DEBUG)
        << printBlockInfo()
        << " local statemachine failed with exception: " << ex.what();
    if (_prefetchTask && !_prefetchTask->isConsumed()) {
      if (!_prefetchTask->tryClaim()) {
        _prefetchTask->waitFor();
      } else {
        _prefetchTask->discard(/*isFinished*/ false);
      }
    }
    THROW_ARANGO_EXCEPTION(_firstFailure);
  } catch (std::exception const& ex) {
    TRI_ASSERT(_firstFailure.ok());
    // store only the first failure we got
    setFailure({TRI_ERROR_INTERNAL,
                absl::StrCat(ex.what(), " [node #", getPlanNode()->id().id(),
                             ": ", getPlanNode()->getTypeString(), "]")});
    LOG_QUERY("2bbd5", DEBUG)
        << printBlockInfo()
        << " local statemachine failed with exception: " << ex.what();
    if (_prefetchTask && !_prefetchTask->isConsumed()) {
      if (!_prefetchTask->tryClaim()) {
        _prefetchTask->waitFor();
      } else {
        _prefetchTask->discard(/*isFinished*/ false);
      }
    }
    // Rewire the error, to be consistent with potentially next caller.
    THROW_ARANGO_EXCEPTION(_firstFailure);
  }
}

// TODO: We need to define the size of this block based on Input / Executor /
// Subquery depth
template<class Executor>
auto ExecutionBlockImpl<Executor>::allocateOutputBlock(AqlCall&& call)
    -> std::unique_ptr<OutputAqlItemRow> {
  if constexpr (Executor::Properties::allowsBlockPassthrough ==
                BlockPassthrough::Enable) {
    // Passthrough variant, re-use the block stored in InputRange
    if (!_hasUsedDataRangeBlock) {
      // In the pass through variant we have the contract that we work on a
      // block all or nothing, so if we have used the block once, we cannot use
      // it again however we cannot remove the _lastRange as it may contain
      // additional information.
      _hasUsedDataRangeBlock = true;
      return createOutputRow(_lastRange.getBlock(), std::move(call));
    }

    return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
  } else {
    if constexpr (isMultiDepExecutor<Executor>) {
      // MultiDepExecutor would require dependency handling.
      // We do not have it here.
      if (!_lastRange.hasValidRow()) {
        // On empty input do not yet create output.
        // We are going to ask again later
        return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
      }
    } else {
      if (!_lastRange.hasValidRow() &&
          _lastRange.upstreamState() == ExecutorState::HASMORE) {
        // On empty input do not yet create output.
        // We are going to ask again later
        return createOutputRow(SharedAqlItemBlockPtr{nullptr}, std::move(call));
      }
    }

    // Non-Passthrough variant, we need to allocate the block ourselfs
    size_t blockSize = ExecutionBlock::DefaultBatchSize;
    if constexpr (hasExpectedNumberOfRows<Executor>::value) {
      // Only limit the output size if there will be no more
      // data from upstream. Or if we have ordered a SOFT LIMIT.
      // Otherwise we will overallocate here.
      // In production it is now very unlikely in the non-softlimit case
      // that the upstream is no block using less than batchSize many rows, but
      // returns HASMORE.
      if (_lastRange.finalState() == MainQueryState::DONE ||
          call.hasSoftLimit()) {
        blockSize = executor().expectedNumberOfRows(_lastRange, call);
        if (_lastRange.finalState() == MainQueryState::HASMORE) {
          // There might be more from above!
          blockSize = std::max(call.getLimit(), blockSize);
        }

        auto const numShadowRows = _lastRange.countShadowRows();
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        // The executor cannot expect to produce more then the limit!
        if constexpr (!std::is_same_v<Executor, SubqueryStartExecutor>) {
          // Except the subqueryStartExecutor, it's limit differs
          // from it's output (it needs to count the new ShadowRows in addition)
          // This however is only correct, as long as we are in no subquery
          // context
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
    return createOutputRow(
        _engine->itemBlockManager().requestBlock(
            blockSize, _registerInfos.numberOfOutputRegisters()),
        std::move(call));
  }
}

template<class Executor>
void ExecutionBlockImpl<Executor>::ensureOutputBlock(AqlCall&& call) {
  if (_outputItemRow == nullptr || !_outputItemRow->isInitialized()) {
    _outputItemRow = allocateOutputBlock(std::move(call));
    TRI_ASSERT(_outputItemRow->numRowsLeft() ==
               std::min(_outputItemRow->blockNumRows(),
                        _outputItemRow->getClientCall().getLimit()))
        << "output numRowsLeft: " << _outputItemRow->numRowsLeft()
        << ", blockNumRows: " << _outputItemRow->blockNumRows()
        << ", call: " << _outputItemRow->getClientCall();
  } else {
    _outputItemRow->setCall(std::move(call));
  }
}

// This cannot return upstream call or shadowrows.
template<class Executor>
auto ExecutionBlockImpl<Executor>::nextState(AqlCall const& call) const
    -> ExecState {
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
template<class Executor>
SharedAqlItemBlockPtr ExecutionBlockImpl<Executor>::requestBlock(
    size_t nrItems, RegisterCount nrRegs) {
  return _engine->itemBlockManager().requestBlock(nrItems, nrRegs);
}

//
// FETCHER:  if we have one output row per input row, we can skip
//           directly by just calling the fetcher and see whether
//           it produced any output.
//           With the new architecture we should be able to just skip
//           ahead on the input range, fetching new blocks when necessary
// EXECUTOR: the executor has a specialized skipRowsRange method
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

namespace arangodb::aql {

template<typename ExecutionTraits>
class IResearchViewExecutor;

template<typename ExecutionTraits>
class IResearchViewMergeExecutor;

template<typename ExecutionTraits>
class IResearchViewHeapSortExecutor;

template<typename T>
struct IsSearchExecutor : std::false_type {};

template<typename ExecutionTraits>
struct IsSearchExecutor<IResearchViewExecutor<ExecutionTraits>>
    : std::true_type {};

template<typename ExecutionTraits>
struct IsSearchExecutor<IResearchViewMergeExecutor<ExecutionTraits>>
    : std::true_type {};

template<typename ExecutionTraits>
struct IsSearchExecutor<IResearchViewHeapSortExecutor<ExecutionTraits>>
    : std::true_type {};

}  // namespace arangodb::aql

// This function is just copy&pasted from above to decide which variant of
// skip is used for which executor.
template<class Executor>
static SkipRowsRangeVariant constexpr skipRowsType() {
  bool constexpr useFetcher =
      Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Enable;

  bool constexpr useExecutor = hasSkipRowsRange<Executor>::value;

  // ConstFetcher and SingleRowFetcher<BlockPassthrough::Enable> can skip, but
  // it may not be done for modification subqueries.
  static_assert(
      useFetcher ==
          (std::is_same_v<typename Executor::Fetcher, ConstFetcher> ||
           (std::is_same_v<typename Executor::Fetcher,
                           SingleRowFetcher<BlockPassthrough::Enable>>)),
      "Unexpected fetcher for SkipVariants::FETCHER");

  static_assert(
      useExecutor ==
              (is_one_of_v<
                  Executor, FilterExecutor, ShortestPathExecutor<ShortestPath>,
                  ShortestPathExecutor<ShortestPathTracer>,
                  ShortestPathExecutor<ShortestPathCluster>,
                  ShortestPathExecutor<ShortestPathClusterTracer>,
                  ShortestPathExecutor<WeightedShortestPath>,
                  ShortestPathExecutor<WeightedShortestPathTracer>,
                  ShortestPathExecutor<WeightedShortestPathCluster>,
                  ShortestPathExecutor<WeightedShortestPathClusterTracer>,
                  ReturnExecutor, EnumeratePathsExecutor<KPath>,
                  EnumeratePathsExecutor<KPathTracer>,
                  EnumeratePathsExecutor<KPathCluster>,
                  EnumeratePathsExecutor<KPathClusterTracer>,
                  EnumeratePathsExecutor<AllShortestPaths>,
                  EnumeratePathsExecutor<AllShortestPathsTracer>,
                  EnumeratePathsExecutor<AllShortestPathsCluster>,
                  EnumeratePathsExecutor<AllShortestPathsClusterTracer>,
                  EnumeratePathsExecutor<KShortestPaths>,
                  EnumeratePathsExecutor<KShortestPathsTracer>,
                  EnumeratePathsExecutor<KShortestPathsCluster>,
                  EnumeratePathsExecutor<KShortestPathsClusterTracer>,
                  EnumeratePathsExecutor<WeightedKShortestPaths>,
                  EnumeratePathsExecutor<WeightedKShortestPathsTracer>,
                  EnumeratePathsExecutor<WeightedKShortestPathsCluster>,
                  EnumeratePathsExecutor<WeightedKShortestPathsClusterTracer>,
                  EnumeratePathsExecutor<YenPaths>,
                  EnumeratePathsExecutor<YenPathsTracer>,
                  EnumeratePathsExecutor<YenPathsCluster>,
                  EnumeratePathsExecutor<YenPathsClusterTracer>,
                  EnumeratePathsExecutor<WeightedYenPaths>,
                  EnumeratePathsExecutor<WeightedYenPathsTracer>,
                  EnumeratePathsExecutor<WeightedYenPathsCluster>,
                  EnumeratePathsExecutor<WeightedYenPathsClusterTracer>,
                  ParallelUnsortedGatherExecutor, JoinExecutor,
                  IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>,
                  IdExecutor<ConstFetcher>, HashedCollectExecutor,
                  AccuWindowExecutor, WindowExecutor, IndexExecutor,
                  EnumerateCollectionExecutor, DistinctCollectExecutor,
                  ConstrainedSortExecutor, CountCollectExecutor,
                  GroupedSortExecutor,
#ifdef ARANGODB_USE_GOOGLE_TESTS
                  TestLambdaSkipExecutor,
#endif
                  ModificationExecutor<
                      SingleRowFetcher<BlockPassthrough::Disable>,
                      InsertModifier>,
                  ModificationExecutor<
                      SingleRowFetcher<BlockPassthrough::Disable>,
                      RemoveModifier>,
                  ModificationExecutor<
                      SingleRowFetcher<BlockPassthrough::Disable>,
                      UpdateReplaceModifier>,
                  ModificationExecutor<
                      SingleRowFetcher<BlockPassthrough::Disable>,
                      UpsertModifier>,
                  TraversalExecutor, EnumerateListObjectExecutor,
                  SubqueryStartExecutor, SubqueryEndExecutor,
                  SortedCollectExecutor, LimitExecutor, UnsortedGatherExecutor,
                  SortingGatherExecutor, SortExecutor, TraversalExecutor,
                  EnumerateListExecutor, SubqueryStartExecutor,
                  SubqueryEndExecutor, SortedCollectExecutor, LimitExecutor,
                  NoResultsExecutor, SingleRemoteModificationExecutor<IndexTag>,
                  SingleRemoteModificationExecutor<Insert>,
                  SingleRemoteModificationExecutor<Remove>,
                  SingleRemoteModificationExecutor<Update>,
                  SingleRemoteModificationExecutor<Replace>,
                  SingleRemoteModificationExecutor<Upsert>,
                  MultipleRemoteModificationExecutor, SortExecutor,
                  EnumerateNearVectorsExecutor, IndexDistinctScanExecutor,
                  IndexAggregateScanExecutor,
                  // only available in Enterprise
                  arangodb::iresearch::OffsetMaterializeExecutor,
                  MaterializeSearchExecutor>) ||
          IsSearchExecutor<Executor>::value,
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
template<class T>
struct dependent_false : std::false_type {};

/**
 * @brief Define the variant of FastForward behaviour
 *
 * FULLCOUNT => Call executeSkipRowsRange and report what has been skipped.
 * EXECUTOR => Call executeSkipRowsRange, but do not report what has been
 * skipped. (This instance is used to make sure Modifications are performed, or
 * stats are correct) FETCHER => Do not bother the Executor, drop all from
 * input, without further reporting
 */
enum class FastForwardVariant { FULLCOUNT, EXECUTOR, FETCHER };

template<class Executor>
static auto fastForwardType(AqlCall const& call, Executor const&)
    -> FastForwardVariant {
  if (call.needsFullCount() && call.getOffset() == 0 && call.getLimit() == 0) {
    // Only start fullCount after the original call is fulfilled. Otherwise
    // do fast-forward variant
    TRI_ASSERT(call.hasHardLimit());
    return FastForwardVariant::FULLCOUNT;
  }
  // TODO: We only need to do this if the executor is required to call.
  // e.g. Modifications and SubqueryStart will always need to be called. Limit
  // only if it needs to report fullCount
  if constexpr (is_one_of_v<Executor, LimitExecutor, SubqueryStartExecutor> ||
                executorHasSideEffects<Executor> ||
                executorCanReturnWaiting<Executor>) {
    return FastForwardVariant::EXECUTOR;
  }
  return FastForwardVariant::FETCHER;
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executeFetcher(ExecutionContext& ctx,
                                                  AqlCallType const& aqlCall)
    -> std::tuple<ExecutionState, SkipResult, typename Fetcher::DataRange> {
  if (getQuery().killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  double start = -1.0;
  auto profilingGuard = scopeGuard([&]() noexcept {
    _execNodeStats.fetching += currentSteadyClockValue() - start;
  });
  if (_profileLevel >= ProfileLevel::Blocks) {
    // only if profiling is turned on, get current time
    start = currentSteadyClockValue();
  } else {
    // if profiling is turned off, don't get current time, but instead
    // cancel the scopeGuard so we don't unnecessarily call timing
    // functions on exit
    profilingGuard.cancel();
  }

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
    auto const& [state, skipped, ranges] =
        fetcher().execute(ctx.stack, aqlCall);
    for (auto const& [dependency, range] : ranges) {
      _lastRange.setDependency(dependency, range);
    }
    return {state, skipped, _lastRange};

  } else if constexpr (executorHasSideEffects<Executor>) {
    // If the executor has side effects, we cannot bypass any subqueries
    // by skipping them. So we need to fetch all shadow rows in order to
    // trigger this Executor with everthing from above.
    // NOTE: The Executor needs to discard shadowRows, and do the accouting.
    static_assert(std::is_same_v<AqlCall, std::decay_t<decltype(aqlCall)>>);
    auto fetchAllStack = ctx.stack.createEquivalentFetchAllShadowRowsStack();
    fetchAllStack.pushCall(
        createUpstreamCall(aqlCall, ctx.clientCallList.hasMoreCalls()));
    auto res = fetcher().execute(fetchAllStack);
    // Just make sure we did not Skip anything
    TRI_ASSERT(std::get<SkipResult>(res).nothingSkipped());
    return res;
  } else {
    // If we are SubqueryStart, we remove the top element of the stack
    // which belongs to the subquery enclosed by this
    // SubqueryStart and the partnered SubqueryEnd by *not*
    // pushing the upstream request.
    if constexpr (!std::is_same_v<Executor, SubqueryStartExecutor>) {
      ctx.stack.pushCall(createUpstreamCall(std::move(aqlCall),
                                            ctx.clientCallList.hasMoreCalls()));
    }

    auto const result = std::invoke([&]() {
      if (_prefetchTask && !_prefetchTask->isConsumed()) {
        if (!_prefetchTask->tryClaim()) {
          TRI_ASSERT(!_dependencies.empty());
          if (_profileLevel >= ProfileLevel::Blocks) {
            // count the parallel invocation
            _dependencies[0]->stats().parallel += 1;
          }
          // some other thread is currently executing our prefetch task
          // -> wait till it has finished.
          _prefetchTask->waitFor();
          auto result = _prefetchTask->stealResult();
          if (std::get<ExecutionState>(result) == ExecutionState::WAITING) {
            // if we got WAITING, we have to immediately call the fetcher again,
            // because it is possible that we already got a wakeup that got
            // swallowed. If the wakeup has already occurred, this call will
            // return actual data, otherwise we will get another WAITING, but we
            // won't have wasted a lot of CPU cycles.
            return fetcher().execute(ctx.stack);
          }

          if (_profileLevel >= ProfileLevel::TraceOne) {
            auto const queryId = this->_engine->getQuery().id();
            LOG_TOPIC("14d20", INFO, Logger::QUERIES)
                << "[query#" << queryId << "] "
                << "returning prefetched result type="
                << getPlanNode()->getTypeString() << " this=" << (uintptr_t)this
                << " id=" << getPlanNode()->id();
          }
          return result;
        }

        // we have claimed the task and are executing the fetcher ourselves, so
        // let's reset the task's internals properly.
        _prefetchTask->discard(/*isFinished*/ false);
      }
      return fetcher().execute(ctx.stack);
    });

    // note: SCHEDULER is a nullptr in unit tests
    if (SchedulerFeature::SCHEDULER != nullptr &&
        std::get<ExecutionState>(result) == ExecutionState::HASMORE &&
        _exeNode->isAsyncPrefetchEnabled() && !ctx.clientCall.hasLimit()) {
      // Async prefetching.
      // we can only use async prefetching if the call does not use a limit.
      // this is because otherwise the prefetching could lead to an overfetching
      // of data.
      bool shouldSchedule = false;
      if (_prefetchTask == nullptr) {
        _prefetchTask = std::make_shared<PrefetchTask>(*this, ctx.stack);
        shouldSchedule = true;
      } else {
        shouldSchedule = _prefetchTask->rearmForNextCall(ctx.stack);
      }

      // TODO - we should avoid flooding the queue with too many tasks as that
      // can significantly delay processing of user REST requests.
      // At the moment we may spawn one task per execution node

      if (shouldSchedule) {
        bool queued = SchedulerFeature::SCHEDULER->tryBoundedQueue(
            RequestLane::INTERNAL_LOW,
            [block = this, task = _prefetchTask]() mutable {
              if (!task->tryClaimOrAbandon()) {
                return;
              }
              // task is a copy of the PrefetchTask shared_ptr, and we will only
              // attempt to execute the task if we successfully claimed the
              // task. i.e., it does not matter if this task lingers around in
              // the scheduler queue even after the execution block has been
              // destroyed, because in this case we will not be able to claim
              // the task and simply return early without accessing the block.
              try {
                task->execute();
              } catch (basics::Exception const& ex) {
                task->setFailure(
                    {ex.code(),
                     absl::StrCat(ex.what(), " [node #",
                                  block->getPlanNode()->id().id(), ": ",
                                  block->getPlanNode()->getTypeString(), "]")});
              } catch (std::exception const& ex) {
                task->setFailure(
                    {TRI_ERROR_INTERNAL,
                     absl::StrCat(ex.what(), " [node #",
                                  block->getPlanNode()->id().id(), ": ",
                                  block->getPlanNode()->getTypeString(), "]")});
              }
            });

        if (!queued) {
          // clear prefetch task
          _prefetchTask.reset();
        } else if (_profileLevel >= ProfileLevel::TraceOne) {
          auto const queryId = this->_engine->getQuery().id();
          LOG_TOPIC("cbf44", INFO, Logger::QUERIES)
              << "[query#" << queryId << "] "
              << "queued prefetch task type=" << getPlanNode()->getTypeString()
              << " this=" << (uintptr_t)this << " id=" << getPlanNode()->id();
        }
      }
    }

    if constexpr (!std::is_same_v<Executor, SubqueryStartExecutor>) {
      // As the stack is copied into the fetcher, we need to pop off our call
      // again. If we use other datastructures or moving we may hand over
      // ownership of the stack here instead and no popCall is necessary.
      std::ignore = ctx.stack.popCall();
    } else {
      // Do not pop the call, we did not put it on.
      // However we need it for accounting later.
    }

    return result;
  }
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executeProduceRows(
    typename Fetcher::DataRange& input, OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, typename Executor::Stats, AqlCallType> {
  if constexpr (isMultiDepExecutor<Executor>) {
    TRI_ASSERT(input.numberDependencies() == _dependencies.size());
    return executor().produceRows(input, output);
  } else if constexpr (executorCanReturnWaiting<Executor>) {
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
  } else {
    return executor().produceRows(input, output);
  }
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executeSkipRowsRange(
    typename Fetcher::DataRange& inputRange, AqlCall& call)
    -> std::tuple<ExecutorState, typename Executor::Stats, size_t,
                  AqlCallType> {
  // The skippedRows is a temporary counter used in this function
  // We need to make sure to reset it afterwards.
  auto sg = arangodb::scopeGuard([&]() noexcept { call.resetSkipCount(); });
  if constexpr (skipRowsType<Executor>() == SkipRowsRangeVariant::EXECUTOR) {
    if constexpr (isMultiDepExecutor<Executor>) {
      TRI_ASSERT(inputRange.numberDependencies() == _dependencies.size());
      // If the executor has a method skipRowsRange, to skip outputs.
      // Every non-passthrough executor needs to implement this.
      auto res = executor().skipRowsRange(inputRange, call);
      _executorReturnedDone =
          std::get<ExecutorState>(res) == ExecutorState::DONE;
      return res;
    } else if constexpr (executorCanReturnWaiting<Executor>) {
      TRI_ASSERT(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    } else {
      auto [state, stats, skipped, localCall] =
          executor().skipRowsRange(inputRange, call);
      _executorReturnedDone = state == ExecutorState::DONE;
      return {state, stats, skipped, localCall};
    }
  } else if constexpr (skipRowsType<Executor>() ==
                       SkipRowsRangeVariant::FETCHER) {
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

template<class Executor>
template<class E>
auto ExecutionBlockImpl<Executor>::sideEffectShadowRowForwarding(
    AqlCallStack& stack, SkipResult& skipResult) -> ExecState {
  static_assert(std::is_same_v<Executor, E> &&
                executorHasSideEffects<Executor>);
  if (!stack.needToCountSubquery()) {
    // We need to really produce things here
    // fall back to original version as any other executor.
    auto res = shadowRowForwarding(stack);
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
  uint64_t depthSkippingNow =
      static_cast<uint64_t>(stack.shadowRowDepthToSkip());
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

template<typename Executor>
auto ExecutionBlockImpl<Executor>::shadowRowForwardingSubqueryStart(
    AqlCallStack& stack)
    -> ExecState requires std::same_as<Executor, SubqueryStartExecutor> {
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());

  // The Subquery Start returns DONE after every row.
  // This needs to be resetted as soon as a shadowRow has been produced
  _executorReturnedDone = false;

  if (_lastRange.hasDataRow()) {
    // If we have a dataRow, the executor needs to write it's output.
    // If we get woken up by a dataRow during forwarding of ShadowRows
    // This will return false, and if so we need to call produce instead.
    auto didWrite = executor().produceShadowRow(_lastRange, *_outputItemRow);
    // Need to report that we have written a row in the call

    if (didWrite) {
      auto& subqueryCall = stack.modifyTopCall();
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

    bool const hasDoneNothing =
        _outputItemRow->numRowsWritten() == 0 and _skipped.nothingSkipped();

    TRI_ASSERT(shadowRow.isInitialized());
    _outputItemRow->increaseShadowRowDepth(shadowRow);
    TRI_ASSERT(_outputItemRow->produced());
    _outputItemRow->advanceRow();

    // Count that we have now produced a row in the new depth.
    // Note: We need to increment the depth by one, as the we have increased
    // it while writing into the output by one as well.
    countShadowRowProduced(stack, shadowRow.getDepth() + 1);

    if (_lastRange.hasShadowRow()) {
      return ExecState::SHADOWROWS;
    }
    // If we do not have more shadowRows
    // we need to return.

    auto& subqueryCallList = stack.modifyCallListAtDepth(shadowRow.getDepth());

    if (!subqueryCallList.hasDefaultCalls()) {
      return ExecState::DONE;
    }

    auto& subqueryCall = subqueryCallList.modifyNextCall();
    if (subqueryCall.getLimit() == 0 && !subqueryCall.needSkipMore()) {
      return ExecState::DONE;
    }

    _executorReturnedDone = false;

    if (hasDoneNothing) {
      stack.popDepthsLowerThan(shadowRow.getDepth());
    }

    return ExecState::NEXTSUBQUERY;
  }
}

template<typename Executor>
auto ExecutionBlockImpl<Executor>::shadowRowForwardingSubqueryEnd(
    AqlCallStack& stack)
    -> ExecState requires std::same_as<Executor, SubqueryEndExecutor> {
  TRI_ASSERT(_outputItemRow);
  TRI_ASSERT(_outputItemRow->isInitialized());
  TRI_ASSERT(!_outputItemRow->allRowsUsed());
  if (!_lastRange.hasShadowRow()) {
    // We got back without a ShadowRow in the LastRange
    // Let client call again
    return ExecState::NEXTSUBQUERY;
  }
  bool const hasDoneNothing =
      _outputItemRow->numRowsWritten() == 0 and _skipped.nothingSkipped();

  auto&& [state, shadowRow] = _lastRange.nextShadowRow();
  TRI_ASSERT(shadowRow.isInitialized());
  if (shadowRow.isRelevant()) {
    // We need to consume the row, and write the Aggregate to it.
    executor().consumeShadowRow(shadowRow, *_outputItemRow);
    // we need to reset the ExecutorHasReturnedDone, it will
    // return done after every subquery is fully collected.
    _executorReturnedDone = false;

  } else {
    _outputItemRow->decreaseShadowRowDepth(shadowRow);
  }

  TRI_ASSERT(_outputItemRow->produced());
  _outputItemRow->advanceRow();
  // The stack in use here contains all calls for within the subquery.
  // Hence any inbound subquery needs to be counted on its level

  countShadowRowProduced(stack, shadowRow.getDepth());

  if (state == ExecutorState::DONE) {
    // We have consumed everything, we are
    // Done with this query
    return ExecState::DONE;
  } else if (_lastRange.hasDataRow()) {
    /// NOTE: We do not need popDepthsLowerThan here, as we already
    /// have a new DataRow from upstream, so the upstream
    /// block has decided it is correct to continue.
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
    if (hasDoneNothing && !shadowRow.isRelevant()) {
      stack.popDepthsLowerThan(shadowRow.getDepth());
    }

    // End of input, we are done for now
    // Need to call again
    return ExecState::NEXTSUBQUERY;
  }
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::shadowRowForwarding(AqlCallStack& stack)
    -> ExecState {
  if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
    return shadowRowForwardingSubqueryStart(stack);
  } else if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
    return shadowRowForwardingSubqueryEnd(stack);
  } else {
    TRI_ASSERT(_outputItemRow);
    TRI_ASSERT(_outputItemRow->isInitialized());
    TRI_ASSERT(!_outputItemRow->allRowsUsed());
    if (!_lastRange.hasShadowRow()) {
      // We got back without a ShadowRow in the LastRange
      // Let us continue with the next Subquery
      return ExecState::NEXTSUBQUERY;
    }

    bool const hasDoneNothing =
        _outputItemRow->numRowsWritten() == 0 and _skipped.nothingSkipped();
    auto&& [state, shadowRow] = _lastRange.nextShadowRow();
    TRI_ASSERT(shadowRow.isInitialized());

    // TODO FIXME WARNING THIS IS AN UGLY HACK. PLEASE SOLVE ME IN A MORE
    // SENSIBLE WAY!
    //
    // the row fetcher doesn't know its ranges, the ranges don't know the
    // fetcher
    //
    // ranges synchronize shadow rows, and fetcher synchronizes skipping
    //
    // but there are interactions between the two.
    if constexpr (std::is_same_v<DataRange, MultiAqlItemBlockInputRange>) {
      fetcher().resetDidReturnSubquerySkips(shadowRow.getDepth());
    }

    countShadowRowProduced(stack, shadowRow.getDepth());
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
      /// NOTE: We do not need popDepthsLowerThan here, as we already
      /// have a new DataRow from upstream, so the upstream
      /// block has decided it is correct to continue.
      // Multiple concatenated Subqueries
      return ExecState::NEXTSUBQUERY;
    } else if (_lastRange.hasShadowRow()) {
      // We still have shadowRows.
      auto const& lookAheadRow = _lastRange.peekShadowRow();
      if (lookAheadRow.isRelevant()) {
        // We are starting the NextSubquery here.
        if constexpr (Executor::Properties::allowsBlockPassthrough ==
                      BlockPassthrough::Enable) {
          // TODO: Check if this works with skip forwarding
          return ExecState::SHADOWROWS;
        }
        return ExecState::NEXTSUBQUERY;
      }
      // we need to forward them
      return ExecState::SHADOWROWS;
    } else {
      if (hasDoneNothing && !shadowRow.isRelevant()) {
        stack.popDepthsLowerThan(shadowRow.getDepth());
      }

      // End of input, need to fetch new!
      // Just start with the next subquery.
      // If in doubt the next row will be a shadowRow again,
      // this will be forwarded than.
      return ExecState::NEXTSUBQUERY;
    }
  }
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executeFastForward(
    typename Fetcher::DataRange& inputRange, AqlCall& clientCall)
    -> std::tuple<ExecutorState, typename Executor::Stats, size_t,
                  AqlCallType> {
  auto type = fastForwardType(clientCall, executor());
  switch (type) {
    case FastForwardVariant::FULLCOUNT: {
      LOG_QUERY("cb135", DEBUG) << printTypeInfo() << " apply full count.";
      auto [state, stats, skippedLocal, call] =
          executeSkipRowsRange(_lastRange, clientCall);

      if constexpr (std::is_same_v<DataRange, MultiAqlItemBlockInputRange>) {
        // The executor will have used all rows.
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
      auto [state, stats, skippedLocal, call] =
          executeSkipRowsRange(_lastRange, dummy);

      if constexpr (std::is_same_v<DataRange, MultiAqlItemBlockInputRange>) {
        // The executor will have used all rows.
        // However we need to drop them from the input
        // here.
        inputRange.skipAllRemainingDataRows();
      }

      return {state, stats, 0, call};
    }
    case FastForwardVariant::FETCHER: {
      LOG_QUERY("fa327", DEBUG) << printTypeInfo() << " bypass unused rows.";
      ADB_IGNORE_UNUSED auto const dependency =
          inputRange.skipAllRemainingDataRows();
      auto constexpr fastForwardCall =
          AqlCall{0, false, 0, AqlCall::LimitType::HARD};
      auto const call = std::invoke([&]() -> AqlCallType {
        if constexpr (std::is_same_v<AqlCallType, AqlCall>) {
          return fastForwardCall;
        } else {
          static_assert(std::is_same_v<AqlCallType, AqlCallSet>);

          auto call = AqlCallSet{};
          call.calls.emplace_back(typename AqlCallSet::DepCallPair{
              dependency, AqlCallList{fastForwardCall}});
          return call;
        }
      });

      // TODO We have to ask all dependencies to go forward to the next shadow
      // row
      auto const state = std::invoke(
          [&](auto) {
            if constexpr (std::is_same_v<DataRange,
                                         MultiAqlItemBlockInputRange>) {
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
 * FASTFORWARD again skipping rows, will count skipped rows, if fullCount is
 * requested. UPSTREAM   fetches rows from the upstream executor(s) to be
 * processed by our executor. SHADOWROWS process any shadow rows DONE processing
 * of one output is done. We did handle offset / limit / fullCount without
 * crossing BatchSize limits. This state does not indicate that we are DONE with
 * all input, we are just done with one walk through this statemachine.
 *
 * We progress within the states in the following way:
 *   There is a nextState method that determines the next state based on the
 * call, it can only lead to: SKIP, PRODUCE, FASTFORWAD, DONE
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
 * @tparam Executor The Executor that will implement the logic of what needs to
 * happen to the data
 * @param stack The call stack of lower levels
 * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>
 *        ExecutionState: WAITING -> We wait for IO, secure state, return you
 * will be called again ExecutionState: HASMORE -> We still have data
 *        ExecutionState: DONE -> We do not have any more data, do never call
 * again size_t -> Amount of documents skipped within this one call. (contains
 * offset and fullCount) SharedAqlItemBlockPtr -> The resulting data
 */
template<class Executor>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
ExecutionBlockImpl<Executor>::executeWithoutTrace(
    AqlCallStack const& callStack) {
  // We can only work on a Stack that has valid calls for all levels.
  TRI_ASSERT(callStack.hasAllValidCalls());
  ExecutionContext ctx(*this, callStack);

  ExecutorState localExecutorState = ExecutorState::DONE;

  if constexpr (executorCanReturnWaiting<Executor>) {
    TRI_ASSERT(
        _execState == ExecState::CHECKCALL ||
        _execState == ExecState::SHADOWROWS ||
        _execState == ExecState::UPSTREAM || _execState == ExecState::PRODUCE ||
        _execState == ExecState::SKIP || _execState == ExecState::FASTFORWARD);
  } else {
    // We can only have returned the following internal states
    TRI_ASSERT(_execState == ExecState::CHECKCALL ||
               _execState == ExecState::SHADOWROWS ||
               _execState == ExecState::UPSTREAM);

    // Skip can only be > 0 if we are in upstream cases, or if we got injected a
    // block
    TRI_ASSERT(_skipped.nothingSkipped() || _execState == ExecState::UPSTREAM ||
               (std::is_same_v<Executor, IdExecutor<ConstFetcher>>));
  }

  if constexpr (Executor::Properties::allowsBlockPassthrough ==
                    BlockPassthrough::Disable &&
                !executorHasSideEffects<Executor>) {
    // Passthroughblocks can never leave anything behind.
    // Side-effect: Executors need to Work through everything themselves even if
    // skipped.
    if ((_execState == ExecState::CHECKCALL ||
         _execState == ExecState::SHADOWROWS) &&
        !ctx.stack.empty()) {
      // We need to check inside a subquery if the outer query has been skipped.
      // But we only need to do this if we were not in WAITING state.
      if (ctx.stack.needToSkipSubquery() && _lastRange.hasValidRow()) {
        auto depthToSkip = ctx.stack.shadowRowDepthToSkip();
        auto& shadowCall = ctx.stack.modifyCallAtDepth(depthToSkip);
        // We can never hit an offset on the shadowRow level again,
        // we can only hit this with HARDLIMIT / FULLCOUNT
        TRI_ASSERT(shadowCall.getOffset() == 0);

        // `depthToSkip` is the depth according to our call and output; when
        // calling _lastRange.skipAllShadowRowsOfDepth() in the following, it is
        // applied to our input.
        // For SQS nodes, this needs to be adjusted; in principle we'd just need
        //   depthToSkip += inputDepthOffset;
        // , except depthToSkip is unsigned, and we would get integer
        // underflows. So it's passed to skipAllShadowRowsOfDepth() instead.
        // Note that SubqueryEnd nodes do *not* need this adjustment, as an
        // additional call is pushed to the stack already when the
        // ExecutionContext is constructed at the beginning of
        // executeWithoutTrace, so input and call-stack already align at this
        // point.
        // However, inversely, because SubqueryEnd nodes push another call for
        // the stack to match their input depth, the stack size is off-by-one
        // compared to their output depth, which is i.a. the size of _skipped.
        // Therefore, outputDepthOffset needs to be passed to didSkipSubquery(),
        // as inputDepthOffset is passed to skipAllShadowRowsOfDepth().
        constexpr static int inputDepthOffset = ([]() consteval->int {
          if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
            return -1;
          } else {
            return 0;
          }
        })();
        constexpr static int outputDepthOffset = ([]() consteval->int {
          if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
            return -1;
          } else {
            return 0;
          }
        })();

        auto skipped =
            _lastRange.template skipAllShadowRowsOfDepth<inputDepthOffset>(
                depthToSkip);
        if (shadowCall.needsFullCount()) {
          if constexpr (std::is_same_v<DataRange,
                                       MultiAqlItemBlockInputRange>) {
            fetcher().reportSubqueryFullCounts(depthToSkip, skipped);
            // We need to report exactly one of those values to the _skipped
            // container If we need help from upstream, they report it via
            // `execute` API.
            auto reportedSkip =
                std::min_element(std::begin(skipped), std::end(skipped));
            _skipped.didSkipSubquery<outputDepthOffset>(*reportedSkip,
                                                        depthToSkip);
          } else {
            _skipped.didSkipSubquery<outputDepthOffset>(skipped, depthToSkip);
          }
        }
        if (_lastRange.hasShadowRow()) {
          // Need to handle ShadowRow next
          _execState = ExecState::SHADOWROWS;
        } else {
          _execState = ExecState::CHECKCALL;
        }
        // We need to reset local executor state.
        resetExecutor();
      }
    }
  }

  // In some executors we may write something into the output, but then return
  // waiting. In this case we are not allowed to lose the call we have been
  // working on, we have noted down created or skipped rows in there. The client
  // is disallowed to change her mind anyways so we simply continue to work on
  // the call we already have The guarantee is, if we have returned the block,
  // and modified our local call, then the outputItemRow is not initialized
  if constexpr (!std::is_same_v<Executor, SubqueryEndExecutor>) {
    // The subqueryEndExecutor has handled it above
    if (_outputItemRow != nullptr && _outputItemRow->isInitialized()) {
      ctx.clientCall = _outputItemRow->getClientCall();
    }
  }

  if constexpr (executorHasSideEffects<Executor>) {
    if (!_skipped.nothingSkipped()) {
      // We get woken up on upstream, but we have not reported our
      // local skip value to downstream
      // In the sideEffect executor we need to apply the skip values on the
      // incomming stack, which has not been modified yet.
      // NOTE: We only apply the skipping on subquery level.
      TRI_ASSERT(_skipped.subqueryDepth() == ctx.stack.subqueryLevel() + 1);
      for (size_t i = 0; i < ctx.stack.subqueryLevel(); ++i) {
        // _skipped and stack are off by one, so we need to add 1 to access
        // to _skipped.
        // They are off by one, because the callstack does not contain the
        // call for the current subquery level (what we are working on right
        // now) as this is replaced by whatever the executor would like to ask
        // from upstream. The skip result is complete, and contains all subquery
        // levels + current level.
        auto skippedSub = _skipped.getSkipOnSubqueryLevel(i + 1);
        if (skippedSub > 0) {
          auto& call = ctx.stack.modifyCallAtDepth(i);
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
    TRI_ASSERT(_clientRequest.requestLessDataThan(ctx.clientCall));
    ctx.clientCall = _clientRequest;

    TRI_ASSERT(_stackBeforeWaiting.requestLessDataThan(ctx.stack));
    ctx.stack = _stackBeforeWaiting;
  }

  if constexpr (executorCanReturnWaiting<Executor>) {
    // If state is SKIP, PRODUCE or FASTFORWARD, we were WAITING.
    // The clientCall and call stack must be restored.
    switch (_execState) {
      default:
        break;
      case ExecState::SKIP:
      case ExecState::PRODUCE:
      case ExecState::FASTFORWARD:
        TRI_ASSERT(_clientRequest.requestLessDataThan(ctx.clientCall));
        ctx.clientCall = _clientRequest;
        TRI_ASSERT(_stackBeforeWaiting.requestLessDataThan(ctx.stack));
        ctx.stack = _stackBeforeWaiting;
    }
  }

  auto returnToState = ExecState::CHECKCALL;
  LOG_QUERY("007ac", DEBUG)
      << "starting statemachine of executor " << printBlockInfo();
  while (_execState != ExecState::DONE) {
    // We can never keep state in the skipCounter
    TRI_ASSERT(ctx.clientCall.getSkipCount() == 0);
    switch (_execState) {
      case ExecState::CHECKCALL: {
        LOG_QUERY("cfe46", DEBUG)
            << printTypeInfo() << " determine next action on call "
            << ctx.clientCall;

        if constexpr (executorHasSideEffects<Executor>) {
          // If the executor has sideEffects, and we need to skip the results we
          // would produce here because we actually skip the subquery, we
          // instead do a hardLimit 0 (aka FastForward) call instead to the
          // local Executor
          if (ctx.stack.needToSkipSubquery()) {
            _execState = ExecState::FASTFORWARD;
            break;
          }
        }
        _execState = nextState(ctx.clientCall);
        break;
      }
      case ExecState::SKIP: {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        auto const offsetBefore = ctx.clientCall.getOffset();
        TRI_ASSERT(offsetBefore > 0);
        bool const canPassFullcount =
            ctx.clientCall.getLimit() == 0 && ctx.clientCall.needsFullCount();
#endif
        LOG_QUERY("1f786", DEBUG)
            << printTypeInfo() << " call skipRows " << ctx.clientCall;

        ExecutorState state = ExecutorState::HASMORE;
        typename Executor::Stats stats;
        size_t skippedLocal = 0;
        AqlCallType call{};
        if constexpr (executorCanReturnWaiting<Executor>) {
          auto sg = arangodb::scopeGuard(
              [&]() noexcept { ctx.clientCall.resetSkipCount(); });
          ExecutionState executorState = ExecutionState::HASMORE;
          std::tie(executorState, stats, skippedLocal, call) =
              executor().skipRowsRange(_lastRange, ctx.clientCall);

          if (executorState == ExecutionState::WAITING) {
            // We need to persist the old call before we return.
            // We might have some local accounting to this call.
            _clientRequest = ctx.clientCall;
            // We might also have some local accounting in this stack.
            _stackBeforeWaiting = ctx.stack;
            // We do not return anything in WAITING state, also NOT skipped.
            TRI_ASSERT(skippedLocal == 0);
            return {executorState, SkipResult{}, nullptr};
          } else if (executorState == ExecutionState::DONE) {
            state = ExecutorState::DONE;
          } else {
            state = ExecutorState::HASMORE;
          }
        } else {
          // Execute skipSome
          std::tie(state, stats, skippedLocal, call) =
              executeSkipRowsRange(_lastRange, ctx.clientCall);
        }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        // Assertion: We did skip 'skippedLocal' documents here.
        // This means that they have to be removed from
        // clientCall.getOffset() This has to be done by the Executor
        // calling call.didSkip() accordingly. The LIMIT executor with a
        // LIMIT of 0 can also bypass fullCount here, even if callLimit > 0
        if (canPassFullcount || std::is_same_v<Executor, LimitExecutor>) {
          // In this case we can first skip. But straight after continue with
          // fullCount, so we might skip more
          TRI_ASSERT(ctx.clientCall.getOffset() + skippedLocal >= offsetBefore);
          if (ctx.clientCall.getOffset() + skippedLocal > offsetBefore) {
            // First need to count down offset.
            TRI_ASSERT(ctx.clientCall.getOffset() == 0);
          }
        } else {
          TRI_ASSERT(ctx.clientCall.getOffset() + skippedLocal == offsetBefore);
        }
#endif
        localExecutorState = state;
        _skipped.didSkip(skippedLocal);
        _blockStats += stats;
        // The execute might have modified the client call.
        if (state == ExecutorState::DONE) {
          _execState = ExecState::FASTFORWARD;
        } else if (ctx.clientCall.getOffset() > 0) {
          TRI_ASSERT(_upstreamState != ExecutionState::DONE);
          // We need to request more
          _upstreamRequest = call;
          _execState = ExecState::UPSTREAM;
        } else {
          // We are done with skipping. Skip is not allowed to request more
          _execState = ExecState::CHECKCALL;
        }
        break;
      }
      case ExecState::PRODUCE: {
        // Make sure there's a block allocated and set
        // the call
        TRI_ASSERT(ctx.clientCall.getLimit() > 0);
        TRI_ASSERT(ctx.clientCall.getSkipCount() == 0);

        LOG_QUERY("1f787", DEBUG)
            << printTypeInfo() << " call produceRows " << ctx.clientCall;
        if (outputIsFull()) {
          // We need to be able to write data
          // But maybe the existing block is full here
          // Then we need to wake up again.
          // However the client might decide on a different
          // call, so we do not record this position
          _execState = ExecState::DONE;
          break;
        }

        if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          TRI_ASSERT(!ctx.stack.empty());
          AqlCall subqueryCall = ctx.stack.peek();
          ensureOutputBlock(std::move(subqueryCall));
        } else {
          ensureOutputBlock(std::move(ctx.clientCall));
        }
        TRI_ASSERT(_outputItemRow);

        TRI_ASSERT(!_executorReturnedDone);
        ExecutorState state = ExecutorState::HASMORE;
        typename Executor::Stats stats;
        auto call = AqlCallType{};
        if constexpr (executorCanReturnWaiting<Executor>) {
          ExecutionState executorState = ExecutionState::HASMORE;
          std::tie(executorState, stats, call) =
              executor().produceRows(_lastRange, *_outputItemRow);

          if (executorState == ExecutionState::WAITING) {
            // We need to persist the old call before we return.
            // We might have some local accounting to this call.
            _clientRequest = ctx.clientCall;
            // We might have some local accounting in this stack.
            _stackBeforeWaiting = ctx.stack;
            // We do not return anything in WAITING state, also NOT skipped.
            return {executorState, SkipResult{}, nullptr};
          } else if (executorState == ExecutionState::DONE) {
            state = ExecutorState::DONE;
          } else {
            state = ExecutorState::HASMORE;
          }
        } else {
          // Execute getSome
          std::tie(state, stats, call) =
              executeProduceRows(_lastRange, *_outputItemRow);
        }
        _executorReturnedDone = state == ExecutorState::DONE;
        _blockStats += stats;
        localExecutorState = state;

        if constexpr (!std::is_same_v<Executor, SubqueryEndExecutor>) {
          // But only do this if we are not subquery.
          ctx.clientCall = _outputItemRow->getClientCall();
        }

        if (state == ExecutorState::DONE) {
          _execState = ExecState::FASTFORWARD;
        } else if ((Executor::Properties::allowsBlockPassthrough ==
                        BlockPassthrough::Enable ||
                    ctx.clientCall.getLimit() > 0) &&
                   outputIsFull()) {
          // In pass through variant we need to stop whenever the block is full.
          // In all other branches only if the client still needs more data.
          _execState = ExecState::DONE;
          break;
        } else if (ctx.clientCall.getLimit() > 0 && executorNeedsCall(call)) {
          TRI_ASSERT(_upstreamState != ExecutionState::DONE);
          // We need to request more
          _upstreamRequest = call;
          _execState = ExecState::UPSTREAM;
        } else {
          // We are done with producing. Produce is not allowed to request more
          _execState = ExecState::CHECKCALL;
        }
        break;
      }
      case ExecState::FASTFORWARD: {
        LOG_QUERY("96e2c", DEBUG)
            << printTypeInfo()
            << " all produced, fast forward to end up (sub-)query.";

        AqlCall callCopy = ctx.clientCall;
        auto sg = arangodb::scopeGuard(
            [&]() noexcept { ctx.clientCall.resetSkipCount(); });
        if constexpr (executorHasSideEffects<Executor>) {
          if (ctx.stack.needToSkipSubquery()) {
            // Fast Forward call.
            callCopy = AqlCall{0, false, 0, AqlCall::LimitType::HARD};
          }
        }

        ExecutorState state = ExecutorState::HASMORE;
        typename Executor::Stats stats;
        size_t skippedLocal = 0;
        AqlCallType call{};

        if constexpr (executorCanReturnWaiting<Executor>) {
          ExecutionState executorState = ExecutionState::HASMORE;

          AqlCall dummy;
          dummy.hardLimit = 0u;
          dummy.fullCount = true;
          std::tie(executorState, stats, skippedLocal, call) =
              executor().skipRowsRange(_lastRange, dummy);
          if (executorState == ExecutionState::WAITING) {
            // We need to persist the old call before we return.
            // We might have some local accounting to this call.
            _clientRequest = ctx.clientCall;
            // We might have some local accounting in this stack.
            _stackBeforeWaiting = ctx.stack;
            // We do not return anything in WAITING state, also NOT skipped.
            TRI_ASSERT(skippedLocal == 0);
            return {executorState, SkipResult{}, nullptr};
          } else if (executorState == ExecutionState::DONE) {
            state = ExecutorState::DONE;
          } else {
            state = ExecutorState::HASMORE;
          }

          if (!callCopy.needsFullCount()) {
            // We forget that we skipped
            skippedLocal = 0;
          }
        } else {
          // Execute skipSome
          std::tie(state, stats, skippedLocal, call) =
              executeFastForward(_lastRange, callCopy);
        }

        if constexpr (executorHasSideEffects<Executor>) {
          if (!ctx.stack.needToSkipSubquery()) {
            // We need to modify the original call.
            ctx.clientCall = callCopy;
          }
          // else: We are bypassing the results.
          // Do not count them here.
        } else {
          ctx.clientCall = callCopy;
        }

        _skipped.didSkip(skippedLocal);
        _blockStats += stats;
        localExecutorState = state;

        if (state == ExecutorState::DONE) {
          if (!_lastRange.hasValidRow()) {
            _execState = ExecState::DONE;
          } else {
            _execState = ExecState::SHADOWROWS;
          }
        } else {
          // We need to request more
          _upstreamRequest = call;
          _execState = ExecState::UPSTREAM;
        }
        break;
      }
      case ExecState::UPSTREAM: {
        LOG_QUERY("488de", DEBUG)
            << printTypeInfo() << " request dependency " << _upstreamRequest;
        // If this triggers the executors produceRows function has returned
        // HASMORE even if it knew that upstream has no further rows.
        TRI_ASSERT(_upstreamState != ExecutionState::DONE);
        // We need to make sure _lastRange is all used for single-dependency
        // executors.
        TRI_ASSERT(isMultiDepExecutor<Executor> || !lastRangeHasDataRow());
        TRI_ASSERT(!_lastRange.hasShadowRow());

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        auto subqueryLevelBefore = ctx.stack.subqueryLevel();
#endif
        SkipResult skippedLocal;
        if (_callstackSplit) {
          // we need to split the callstack to avoid stack overflows, so we move
          // upstream execution into a separate thread
          std::tie(_upstreamState, skippedLocal, _lastRange) =
              _callstackSplit->execute(ctx, _upstreamRequest);
        } else {
          std::tie(_upstreamState, skippedLocal, _lastRange) =
              executeFetcher(ctx, _upstreamRequest);
        }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        TRI_ASSERT(subqueryLevelBefore == ctx.stack.subqueryLevel());
#endif

        if (_upstreamState == ExecutionState::WAITING) {
          // We need to persist the old call before we return.
          // We might have some local accounting to this call.
          _clientRequest = ctx.clientCall;
          // We might also have some local accounting in this stack.
          _stackBeforeWaiting = ctx.stack;
          // We do not return anything in WAITING state, also NOT skipped.
          return {_upstreamState, SkipResult{}, nullptr};
        }

        if (!skippedLocal.nothingSkipped()) {
          if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
            // In SubqueryStart the stack is exactly the same size as the skip
            // result from above, the call we work on is inside the subquery.
            // The stack is exactly what we send upstream, no added call on top.
            TRI_ASSERT(skippedLocal.subqueryDepth() ==
                       ctx.stack.subqueryLevel());
            for (size_t i = 0; i < ctx.stack.subqueryLevel(); ++i) {
              auto skippedSub = skippedLocal.getSkipOnSubqueryLevel(i);
              if (skippedSub > 0) {
                auto& call = ctx.stack.modifyCallAtDepth(i);
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
            TRI_ASSERT(skippedLocal.subqueryDepth() ==
                       ctx.stack.subqueryLevel() + 1);

            for (size_t i = 0; i < ctx.stack.subqueryLevel(); ++i) {
              auto skippedSub = skippedLocal.getSkipOnSubqueryLevel(i + 1);
              if (skippedSub > 0) {
                auto& call = ctx.stack.modifyCallAtDepth(i);
                call.didSkip(skippedSub);
                call.resetSkipCount();
              }
            }
          }
        }

        if constexpr (Executor::Properties::allowsBlockPassthrough ==
                      BlockPassthrough::Enable) {
          // We have a new range, passthrough can use this range.
          _hasUsedDataRangeBlock = false;
        }

        if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          // We need to pop the last subquery from the returned skip
          // We have not asked for a subquery skip.
          TRI_ASSERT(skippedLocal.getSkipCount() == 0);
          skippedLocal.decrementSubquery();
        }
        if constexpr (skipRowsType<Executor>() ==
                      SkipRowsRangeVariant::FETCHER) {
          // We skipped through passthrough, so count that a skip was solved.
          _skipped.merge(skippedLocal, false);
          ctx.clientCall.didSkip(skippedLocal.getSkipCount());
          ctx.clientCall.resetSkipCount();
        } else if constexpr (is_one_of_v<Executor, SubqueryStartExecutor,
                                         SubqueryEndExecutor>) {
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
            TRI_ASSERT(_skipped.subqueryDepth() ==
                       skippedLocal.subqueryDepth());
            _skipped.incrementSubquery();
          }
        }

        if (_lastRange.hasShadowRow() &&
            !_lastRange.peekShadowRow().isRelevant()) {
          // we do not have any input for this executor on the current depth.
          // We have skipped over the full subquery execution, so claim it is
          // DONE for now. It will be resetted after this shadowRow. If one
          // of the next SubqueryRuns is not skipped over.
          localExecutorState = ExecutorState::DONE;
          _execState = ExecState::SHADOWROWS;
          // The following line is in particular for the UnsortedGatherExecutor,
          // which implies we're using a MultiDependencyRowFetcher.
          // Imagine the following situation:
          // In case the last subquery ended, at least for one dependency, on an
          // item block-boundary. But the next row in this dependency - and
          // thus, all other dependencies - is a non-relevant shadow row. Then
          // the executor will have been called by now, possibly multiple times,
          // until all dependencies have some input and thus arrived at this
          // particular shadow row. So now the condition of this if-branch is
          // true.
          // The outcome of this is that the executor is no longer in a freshly
          // initialized state, but may have progressed over one or more
          // dependencies already.
          // Without the following reset, these dependencies would thus be
          // ignored in the next subquery iteration.
          resetExecutor();
        } else {
          _execState = ExecState::CHECKCALL;
        }
        break;
      }
      case ExecState::SHADOWROWS: {
        // We only get called with something in the input.
        TRI_ASSERT(_lastRange.hasValidRow());
        LOG_QUERY("7c63c", DEBUG)
            << printTypeInfo() << " (sub-)query completed. Move ShadowRows.";

        // TODO: Check if we can have the situation that we are between two
        // shadow rows here. E.g. LastRow is relevant shadowRow. NextRow is
        // non-relevant shadowRow. NOTE: I do not think this is an issue, as the
        // Executor will always say that it cannot do anything with an empty
        // input. Only exception might be COLLECT COUNT.

        if (outputIsFull()) {
          // We need to be able to write data
          // But maybe the existing block is full here
          // Then we need to wake up again here.
          returnToState = ExecState::SHADOWROWS;
          _execState = ExecState::DONE;
          break;
        }

        if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
          TRI_ASSERT(!ctx.stack.empty());
          // unfortunately we cannot move here, because clientCall can still be
          // read-from later
          AqlCall copyCall = ctx.clientCall;
          ensureOutputBlock(std::move(copyCall));
        } else {
          ensureOutputBlock(std::move(ctx.clientCall));
        }

        TRI_ASSERT(!_outputItemRow->allRowsUsed());
        if constexpr (executorHasSideEffects<Executor>) {
          _execState = sideEffectShadowRowForwarding(ctx.stack, _skipped);
        } else {
          // This may write one or more rows.
          _execState = shadowRowForwarding(ctx.stack);
        }
        if constexpr (!std::is_same_v<Executor, SubqueryEndExecutor>) {
          // Produce might have modified the clientCall
          // But only do this if we are not subquery.
          ctx.clientCall = _outputItemRow->getClientCall();
        }
        break;
      }
      case ExecState::NEXTSUBQUERY: {
        // This state will continue with the next run in the current subquery.
        // For this executor the input of the next run will be injected and it
        // can continue to work.
        LOG_QUERY("0ca35", DEBUG)
            << printTypeInfo()
            << " ShadowRows moved, continue with next subquery.";

        if (!ctx.stack.hasAllValidCalls()) {
          // We can only continue if we still have a valid call
          // on all levels
          _execState = ExecState::DONE;
          break;
        }

        if constexpr (std::is_same_v<Executor, SubqueryStartExecutor>) {
          auto currentSubqueryCall = ctx.stack.peek();
          if (currentSubqueryCall.getLimit() == 0 &&
              currentSubqueryCall.hasSoftLimit()) {
            // SoftLimitReached.
            // We cannot continue.
            _execState = ExecState::DONE;
            break;
          }
          // Otherwise just check like the other blocks
        }

        if (ctx.clientCallList.hasMoreCalls()) {
          // Update to next call and start all over.
          ctx.clientCall = ctx.clientCallList.popNextCall();
          _execState = ExecState::CHECKCALL;
        } else {
          // We cannot continue, so we are done
          _execState = ExecState::DONE;
        }

        break;
      }
      default:
        // unreachable
        TRI_ASSERT(false);
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    }
    TRI_ASSERT(ctx.clientCall.getSkipCount() == 0);
  }
  LOG_QUERY("80c24", DEBUG)
      << printBlockInfo() << " local statemachine done. Return now.";
  // If we do not have an output, we simply return a nullptr here.

  if constexpr (Executor::Properties::allowsBlockPassthrough ==
                BlockPassthrough::Enable) {
    // We can never return less rows than what we got!
    TRI_ASSERT(_outputItemRow == nullptr || _outputItemRow->numRowsLeft() == 0)
        << printBlockInfo() << " Passthrough block didn't process all rows. "
        << (_outputItemRow == nullptr
                ? fmt::format("output == nullptr")
                : fmt::format("rows left = {}, rows written = {}",
                              _outputItemRow->numRowsLeft(),
                              _outputItemRow->numRowsWritten()));
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
  if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
    TRI_ASSERT(skipped.subqueryDepth() ==
               ctx.stack.subqueryLevel() /*we injected a call*/);
  } else {
    TRI_ASSERT(skipped.subqueryDepth() ==
               ctx.stack.subqueryLevel() + 1 /*we took our call*/)
        << printBlockInfo()
        << " skipped.subqueryDepth() = " << skipped.subqueryDepth()
        << ", ctx.stack.subqueryLevel() + 1 = "
        << ctx.stack.subqueryLevel() + 1;
  }
#endif
  _skipped.reset();
  if (localExecutorState == ExecutorState::HASMORE || _lastRange.hasDataRow() ||
      _lastRange.hasShadowRow()) {
    // We have skipped or/and returned data, otherwise we cannot return HASMORE
    TRI_ASSERT(!skipped.nothingSkipped() ||
               (outputBlock != nullptr && outputBlock->numRows() > 0));
    return {ExecutionState::HASMORE, skipped, std::move(outputBlock)};
  }
  // We must return skipped and/or data when reporting HASMORE
  TRI_ASSERT(_upstreamState != ExecutionState::HASMORE ||
             (!skipped.nothingSkipped() ||
              (outputBlock != nullptr && outputBlock->numRows() > 0)));

  return {_upstreamState, skipped, std::move(outputBlock)};
}

template<class Executor>
void ExecutionBlockImpl<Executor>::resetExecutor() {
  // cppcheck-suppress unreadVariable
  constexpr bool customInit = hasInitializeCursor<Executor>::value;
  // IndexExecutor and EnumerateCollectionExecutor have initializeCursor
  // implemented, so assert this implementation is used.
  static_assert(
      !std::is_same<Executor, EnumerateCollectionExecutor>::value || customInit,
      "EnumerateCollectionExecutor is expected to implement a custom "
      "initializeCursor method!");
  static_assert(!std::is_same<Executor, IndexExecutor>::value || customInit,
                "IndexExecutor is expected to implement a custom "
                "initializeCursor method!");
  static_assert(
      !std::is_same<Executor, DistinctCollectExecutor>::value || customInit,
      "DistinctCollectExecutor is expected to implement a custom "
      "initializeCursor method!");

  if constexpr (customInit) {
    TRI_ASSERT(_executor.has_value());
    // re-initialize the Executor
    _executor->initializeCursor();
  } else {
    // destroy and re-create the Executor
    _executor.emplace(fetcher(), _executorInfos);
  }

  _executorReturnedDone = false;
}

template<class Executor>
void ExecutionBlockImpl<Executor>::setFailure(Result&& res) {
  _firstFailure = std::move(res);
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::outputIsFull() const noexcept -> bool {
  return _outputItemRow != nullptr && _outputItemRow->isInitialized() &&
         _outputItemRow->allRowsUsed();
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::lastRangeHasDataRow() const noexcept
    -> bool {
  return _lastRange.hasDataRow();
}

template<class Executor>
void ExecutionBlockImpl<Executor>::init() {
  TRI_ASSERT(!_initialized);
  if constexpr (isMultiDepExecutor<Executor>) {
    _lastRange.resizeOnce(MainQueryState::HASMORE, 0, _dependencies.size());
    fetcher().init();
  }
}

template<class Executor>
void ExecutionBlockImpl<Executor>::initOnce() {
  if (!_initialized) {
    init();
    _initialized = true;
  }
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::executorNeedsCall(
    AqlCallType& call) const noexcept -> bool {
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

template<class Executor>
auto ExecutionBlockImpl<Executor>::memoizeCall(
    AqlCall const& call, bool wasCalledWithContinueCall) noexcept -> void {
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

template<class Executor>
auto ExecutionBlockImpl<Executor>::createUpstreamCall(
    AqlCall const& call, bool wasCalledWithContinueCall) -> AqlCallList {
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

template<class Executor>
auto ExecutionBlockImpl<Executor>::countShadowRowProduced(AqlCallStack& stack,
                                                          size_t depth)
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

#ifdef ARANGODB_USE_GOOGLE_TESTS
// This is a helper method to inject a prepared
// input range in the tests. It should simulate
// an ongoing query in a specific state.
template<class Executor>
auto ExecutionBlockImpl<Executor>::testInjectInputRange(DataRange range,
                                                        SkipResult skipped)
    -> void {
  if (range.finalState() == MainQueryState::DONE) {
    _upstreamState = ExecutionState::DONE;
  } else {
    _upstreamState = ExecutionState::HASMORE;
  }
  _lastRange = std::move(range);
  _skipped = skipped;
  if constexpr (std::is_same_v<Fetcher, MultiDependencySingleRowFetcher>) {
    // Make sure we have initialized the Fetcher / Dependencies properly
    initOnce();
    // Now we need to initialize the SkipCounts, to simulate that something
    // was skipped.
    fetcher().initialize(skipped.subqueryDepth());
  }
}
#endif

template<class Executor>
ExecutionBlockImpl<Executor>::ExecutionContext::ExecutionContext(
    ExecutionBlockImpl& block, AqlCallStack const& callstack)
    : stack(callstack), clientCallList(this->stack.popCall()) {
  if constexpr (std::is_same_v<Executor, SubqueryEndExecutor>) {
    // In subqeryEndExecutor we actually manage two calls.
    // The clientCall defines what will go into the Executor.
    // on SubqueryEnd this call is generated based on the call from downstream

    if (block._outputItemRow != nullptr &&
        block._outputItemRow->isInitialized()) {
      // If we return with a waiting state, we need to report it to the
      // subquery callList, but not pull it of.
      auto& subQueryCall = clientCallList.modifyNextCall();
      // Overwrite with old state.
      subQueryCall = block._outputItemRow->getClientCall();
    }
    stack.pushCall(std::move(clientCallList));
    clientCallList = AqlCallList{AqlCall{}, AqlCall{}};
  }
  clientCall = clientCallList.popNextCall();
  // We got called with a skip count already set!
  // Caller is wrong fix it.
  TRI_ASSERT(clientCall.getSkipCount() == 0);

  TRI_ASSERT(!(clientCall.getOffset() == 0 &&
               clientCall.softLimit == AqlCall::Limit{0u}));
  TRI_ASSERT(!(clientCall.hasSoftLimit() && clientCall.fullCount));
  TRI_ASSERT(!(clientCall.hasSoftLimit() && clientCall.hasHardLimit()));
}

template<class Executor>
bool ExecutionBlockImpl<Executor>::PrefetchTask::isConsumed() const noexcept {
  return _state.load(std::memory_order_relaxed).status == Status::Consumed;
}

template<class Executor>
bool ExecutionBlockImpl<Executor>::PrefetchTask::tryClaim() noexcept {
  auto state = _state.load(std::memory_order_relaxed);
  while (true) {
    if (state.status != Status::Pending) {
      return false;
    }
    if (_state.compare_exchange_strong(state,
                                       {Status::InProgress, state.abandoned},
                                       std::memory_order_relaxed)) {
      return true;
    }
  }
}

template<class Executor>
bool ExecutionBlockImpl<Executor>::PrefetchTask::tryClaimOrAbandon() noexcept {
  auto state = _state.load(std::memory_order_relaxed);
  while (true) {
    // this function is only called from the scheduled task, and we must only
    // schedule one task at a time, so this task must not be abandoned yet!
    TRI_ASSERT(state.abandoned == false);
    if (state.status != Status::Pending) {
      // task is not longer pending, so let's try to abandon it
      // Note: if we fail here it is possible that the task is already rearmed
      // and reset to pending, so we can retry to claim it in the next
      // iteration.
      if (_state.compare_exchange_weak(
              state, {.status = state.status, .abandoned = true},
              std::memory_order_relaxed)) {
        // we successfully abandoned the task, so we can return false to
        // indicate that we must not work on this task
        return false;
      }
    } else {
      TRI_ASSERT(state.status == Status::Pending);
      if (_state.compare_exchange_weak(
              state, {.status = Status::InProgress, .abandoned = false},
              std::memory_order_acquire)) {
        // successfully claimed the task!
        return true;
      }
    }
  }
}

template<class Executor>
bool ExecutionBlockImpl<Executor>::PrefetchTask::rearmForNextCall(
    AqlCallStack const& stack) {
  TRI_ASSERT(!_result);
  _stack = stack;
  // intentionally do not reset _firstFailure
  auto old =
      _state.exchange({Status::Pending, false}, std::memory_order_release);
  TRI_ASSERT(old.status == Status::Consumed);
  // if the task was abandoned, we want to reschedule it!
  return old.abandoned;
}

template<class Executor>
void ExecutionBlockImpl<Executor>::PrefetchTask::waitFor() const noexcept {
  std::unique_lock<std::mutex> guard(_lock);
  // (1) - this acquire-load synchronizes with the release-store (3)
  if (_state.load(std::memory_order_acquire).status == Status::Finished) {
    return;
  }

  _bell.wait(guard, [this]() {
    // (2) - this acquire-load synchronizes with the release-store (3)
    return _state.load(std::memory_order_acquire).status == Status::Finished;
  });
}

template<class Executor>
void ExecutionBlockImpl<Executor>::PrefetchTask::updateStatus(
    Status status, std::memory_order memoryOrder) noexcept {
  auto state = _state.load(std::memory_order_relaxed);
  while (not _state.compare_exchange_weak(
      state, {status, state.abandoned}, memoryOrder, std::memory_order_relaxed))
    ;
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::PrefetchTask::discard(
    bool isFinished) noexcept -> void {
  _result.reset();
  updateStatus(isFinished ? Status::Finished : Status::Consumed,
               std::memory_order_release);
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::PrefetchTask::stealResult()
    -> PrefetchResult {
  TRI_ASSERT(_result || _firstFailure.fail())
      << "prefetch task state: "
      << (int)_state.load(std::memory_order_relaxed).status;
  updateStatus(Status::Consumed, std::memory_order_relaxed);
  if (_firstFailure.fail()) {
    _result.reset();
    THROW_ARANGO_EXCEPTION(_firstFailure);
  }
  auto r = std::move(_result.value());
  _result.reset();
  return r;
}

template<class Executor>
void ExecutionBlockImpl<Executor>::PrefetchTask::execute() {
  if constexpr (std::is_same_v<Fetcher, MultiDependencySingleRowFetcher> ||
                executorHasSideEffects<Executor>) {
    TRI_ASSERT(false);
  } else {
    TRI_ASSERT(_state.load().status == Status::InProgress);
    TRI_ASSERT(!_result);

    _result = _block.fetcher().execute(_stack);

    TRI_ASSERT(_result.has_value());

    wakeupWaiter();
  }
}

template<class Executor>
void ExecutionBlockImpl<Executor>::PrefetchTask::setFailure(Result&& res) {
  TRI_ASSERT(res.fail());
  if (_firstFailure.ok()) {
    _firstFailure = std::move(res);
  }
  _result.reset();
  wakeupWaiter();
}

template<class Executor>
void ExecutionBlockImpl<Executor>::PrefetchTask::wakeupWaiter() noexcept {
  // need to temporarily lock the mutex to enforce serialization with the
  // waiting thread
  _lock.lock();
  // (3) - this release-store synchronizes with the acquire-load (1, 2)
  _state.store({Status::Finished, true}, std::memory_order_release);
  _lock.unlock();

  _bell.notify_one();
}

template<class Executor>
ExecutionBlockImpl<Executor>::CallstackSplit::CallstackSplit(
    ExecutionBlockImpl& block)
    : _block(block),
      _thread(&CallstackSplit::run, this, ExecContext::currentAsShared()) {}

template<class Executor>
ExecutionBlockImpl<Executor>::CallstackSplit::~CallstackSplit() {
  _lock.lock();
  _state.store(State::Stopped);
  _lock.unlock();

  _bell.notify_one();
  _thread.join();
}

template<class Executor>
auto ExecutionBlockImpl<Executor>::CallstackSplit::execute(
    ExecutionContext& ctx, AqlCallType const& aqlCall) -> UpstreamResult {
  std::variant<UpstreamResult, std::exception_ptr, std::nullopt_t> result{
      std::nullopt};
  Params params{result, ctx, aqlCall, LogContext::current()};

  {
    std::unique_lock guard(_lock);
    _params = &params;
    _state.store(State::Executing);
  }

  _bell.notify_one();

  std::unique_lock<std::mutex> guard(_lock);
  _bell.wait(guard, [this]() {
    return _state.load(std::memory_order_acquire) != State::Executing;
  });
  TRI_ASSERT(_state.load() == State::Waiting);

  if (std::holds_alternative<std::exception_ptr>(result)) {
    std::rethrow_exception(std::get<std::exception_ptr>(result));
  }

  return std::get<UpstreamResult>(std::move(result));
}

template<class Executor>
void ExecutionBlockImpl<Executor>::CallstackSplit::run(
    std::shared_ptr<ExecContext const> execContext) {
  ExecContextScope scope(execContext);
  std::unique_lock<std::mutex> guard(_lock);
  while (true) {
    _bell.wait(guard, [this]() {
      return _state.load(std::memory_order_relaxed) != State::Waiting;
    });
    if (_state == State::Stopped) {
      return;
    }
    TRI_ASSERT(_params != nullptr);
    _state = State::Executing;

    LogContext::setCurrent(_params->logContext);
    try {
      _params->result = _block.executeFetcher(_params->ctx, _params->aqlCall);
    } catch (...) {
      _params->result = std::current_exception();
    }

    _state.store(State::Waiting, std::memory_order_relaxed);
    _bell.notify_one();
  }
}

// TODO: find out whether this is still needed. It is used in
//       ScatterExecutor and in DistributeClientExecutor
template<class Executor>
auto ExecutionBlockImpl<Executor>::injectConstantBlock(
    SharedAqlItemBlockPtr block, SkipResult skipped)
    -> void requires std::same_as<Executor, IdExecutor<ConstFetcher>> {
  // reinitialize the DependencyProxy
  _dependencyProxy.reset();

  // destroy and re-create the Fetcher
  _rowFetcher.emplace(_dependencyProxy);

  TRI_ASSERT(_skipped.nothingSkipped());

  // Local skipped is either fresh (depth == 1)
  // Or exactly of the size handed in
  TRI_ASSERT(_skipped.subqueryDepth() == 1 ||
             _skipped.subqueryDepth() == skipped.subqueryDepth());

  TRI_ASSERT(_state == InternalState::DONE ||
             _state == InternalState::FETCH_DATA);

  _state = InternalState::FETCH_DATA;

  // Reset state of execute
  _lastRange = AqlItemBlockInputRange{MainQueryState::HASMORE};
  _hasUsedDataRangeBlock = false;
  _upstreamState = ExecutionState::HASMORE;

  fetcher().injectBlock(std::move(block), std::move(skipped));

  resetExecutor();
}

// FIXME: this might not be used anymore; with the introduction of
// spliced subqueries we shouldn't have the case in
// ExecutionEngine::setupEngineRoot. Trying to ADB_PROD_ASSERT
// this fact leads to instant crash on startup though.
template<typename Executor>
auto ExecutionBlockImpl<Executor>::getOutputRegisterId() const noexcept
    -> RegisterId requires std::same_as<
        Executor, IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>> {
  return _executorInfos.getOutputRegister();
}
