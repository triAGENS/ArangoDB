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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "DistributeClientBlock.h"

#include "Aql/AqlItemBlockManager.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/IdExecutor.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/ShadowAqlItemRow.h"

using namespace arangodb;
using namespace arangodb::aql;

DistributeClientBlock::QueueEntry::QueueEntry(SkipResult const& skipped,
                                              SharedAqlItemBlockPtr block,
                                              std::vector<size_t> chosen)
    : _skip(skipped), _block(std::move(block)), _chosen(std::move(chosen)) {
  TRI_ASSERT(_block != nullptr || _chosen.empty());
}

auto DistributeClientBlock::QueueEntry::numRows() const -> size_t {
  return _chosen.size();
}

auto DistributeClientBlock::QueueEntry::skipResult() const -> SkipResult const& {
  return _skip;
}

auto DistributeClientBlock::QueueEntry::block() const -> SharedAqlItemBlockPtr const& {
  return _block;
}

auto DistributeClientBlock::QueueEntry::chosen() const -> std::vector<size_t> const& {
  return _chosen;
}

DistributeClientBlock::DistributeClientBlock(ExecutionEngine& engine,
                                             ExecutionNode const* node,
                                             RegisterInfos const& registerInfos)
    : _blockManager(engine.itemBlockManager()), registerInfos(registerInfos) {
  // We only get shared ptrs to const data. so we need to copy here...
  auto executorInfos = IdExecutorInfos{false, 0, "", false};
  auto idExecutorRegisterInfos =
      RegisterInfos{{},
                    {},
                    registerInfos.numberOfInputRegisters(),
                    registerInfos.numberOfOutputRegisters(),
                    registerInfos.registersToClear(),
                    registerInfos.registersToKeep()};
  // NOTE: Do never change this type! The execute logic below requires this and only this type.
  _executor = std::make_unique<ExecutionBlockImpl<IdExecutor<ConstFetcher>>>(
      &engine, node, std::move(idExecutorRegisterInfos), std::move(executorInfos));
}

auto DistributeClientBlock::clear() -> void {
  _queue.clear();
  _executorHasMore = false;
}

auto DistributeClientBlock::addBlock(SkipResult const& skipResult, SharedAqlItemBlockPtr block,
                                     std::vector<size_t> usedIndexes) -> void {
  TRI_ASSERT(!usedIndexes.empty() || block == nullptr);
  _queue.emplace_back(skipResult, std::move(block), std::move(usedIndexes));
}

auto DistributeClientBlock::hasDataFor(AqlCall const& call) -> bool {
  return _executorHasMore || !_queue.empty();
}

/**
 * @brief This call will join as many blocks as available from the queue
 *        and return them in a SingleBlock. We then use the IdExecutor
 *        to hand out the data contained in these blocks
 *        We do on purpose not give any kind of guarantees on the sizing of
 *        this block to be flexible with the implementation, and find a good
 *        trade-off between blocksize and block copy operations.
 *
 * @return SharedAqlItemBlockPtr a joind block from the queue.
 */
auto DistributeClientBlock::popJoinedBlock()
    -> std::tuple<SharedAqlItemBlockPtr, SkipResult> {
  // There are some optimizations available in this implementation.
  // Namely we could apply good logic to cut the blocks at shadow rows
  // in order to allow the IDexecutor to hand them out en-block.
  // However we might leverage the restriction to stop at ShadowRows
  // at one point anyways, and this Executor has no business with ShadowRows.
  size_t numRows = 0;
  for (auto const& data : _queue) {
    if (numRows + data.numRows() > ExecutionBlock::DefaultBatchSize) {
      // Avoid to put too many rows into this block.
      break;
    }
    numRows += data.numRows();
  }
  // DO never distribute more then BatchSize many rows.
  TRI_ASSERT(numRows <= ExecutionBlock::DefaultBatchSize);
  SkipResult skipRes;
  if (numRows == 0) {
    // Special case, we do not have any data to distribute.
    // But we still may have skip Information.
    // So we need to distribute at-least this.
    while (!_queue.empty()) {
      auto const& entry = _queue.front();
      skipRes.merge(entry.skipResult(), false);
      TRI_ASSERT(entry.block() == nullptr);
      TRI_ASSERT(entry.chosen().empty());
      _queue.pop_front();
    }
    return {nullptr, skipRes};
  }

  // If we do not have rows, we will get an empty block here.
  // We still need to morege the SkipResults.
  SharedAqlItemBlockPtr newBlock =
      _blockManager.requestBlock(numRows, registerInfos.numberOfOutputRegisters());
  // We create a block, with correct register information
  // but we do not allow outputs to be written.
  RegIdSet const noOutputRegisters{};
  OutputAqlItemRow output{newBlock, noOutputRegisters, registerInfos.registersToKeep(),
                          registerInfos.registersToClear()};
  while (!output.isFull()) {
    // If the queue is empty our sizing above would not be correct
    TRI_ASSERT(!_queue.empty());

    auto const& entry = _queue.front();
    skipRes.merge(entry.skipResult(), true);
    auto const& block = entry.block();
    auto const& chosen = entry.chosen();
    TRI_ASSERT(output.numRowsLeft() >= chosen.size());
    for (auto const& i : chosen) {
      // We do not really care what we copy. However
      // the API requires to know what it is.
      if (block->isShadowRow(i)) {
        ShadowAqlItemRow toCopy{block, i};
        output.moveRow(toCopy);
      } else {
        InputAqlItemRow toCopy{block, i};
        output.copyRow(toCopy);
      }
      output.advanceRow();
    }
    // All required rows copied.
    // Drop block form queue.
    _queue.pop_front();
  }
  return {newBlock, skipRes};
}

auto DistributeClientBlock::execute(AqlCallStack const& callStack, AqlCallList clientCall, ExecutionState upstreamState)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  TRI_ASSERT(_executor != nullptr);
  // Make sure we actually have data before you call execute
  TRI_ASSERT(hasDataFor(clientCall.peekNextCall()));
  if (!_executorHasMore) {
    // This cast is guaranteed, we create this a couple lines above and only
    // this executor is used here.
    // Unfortunately i did not get a version compiled were i could only forward
    // declare the templates in header.
    auto casted =
        static_cast<ExecutionBlockImpl<IdExecutor<ConstFetcher>>*>(_executor.get());
    TRI_ASSERT(casted != nullptr);
    auto [block, skipped] = popJoinedBlock();
    casted->injectConstantBlock(block, skipped);
    _executorHasMore = true;
  }
  auto [state, skipped, result] = _executor->execute(callStack, std::move(clientCall));

  // We have all data locally cannot wait here.
  TRI_ASSERT(state != ExecutionState::WAITING);

  if (state == ExecutionState::DONE) {
    // This executor is finished, including shadowrows
    // We are going to reset it on next call
    _executorHasMore = false;

    // Also we need to adjust the return states
    // as this state only represents one single block
    if (!_queue.empty()) {
      state = ExecutionState::HASMORE;
    } else {
      state = upstreamState;
    }
  }
  return {state, skipped, result};
}
