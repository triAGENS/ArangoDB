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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "AllRowsFetcher.h"
#include <Logger/LogMacros.h>

#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemMatrix.h"
#include "Aql/DependencyProxy.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/ShadowAqlItemRow.h"

using namespace arangodb;
using namespace arangodb::aql;

std::tuple<ExecutionState, SkipResult, AqlItemBlockInputMatrix> AllRowsFetcher::execute(AqlCallStack const& stack, AqlCallList clientCall) {
  TRI_ASSERT(clientCall.peekNextCall().getOffset() == 0);
  TRI_ASSERT(!clientCall.peekNextCall().needsFullCount());
  // We allow a 0 hardLimit for bypassing
  // bot otherwise we do not allow any limit
  TRI_ASSERT(!clientCall.peekNextCall().hasHardLimit() || clientCall.peekNextCall().getLimit() == 0);
  TRI_ASSERT(!clientCall.peekNextCall().hasSoftLimit());

  if (_aqlItemMatrix == nullptr) {
    _aqlItemMatrix = std::make_unique<AqlItemMatrix>(getNrInputRegisters());
  }
  // We can only execute More if we are not Stopped yet.
  TRI_ASSERT(!_aqlItemMatrix->stoppedOnShadowRow());
  while (true) {
    auto [state, skipped, block] = _dependencyProxy->execute(stack, clientCall);
    TRI_ASSERT(skipped.getSkipCount() == 0);

    // we will either build a complete fetched AqlItemBlockInputMatrix or return an empty one
    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(skipped.nothingSkipped());
      TRI_ASSERT(block == nullptr);
      // On waiting we have nothing to return
      return {state, SkipResult{}, AqlItemBlockInputMatrix{ExecutorState::HASMORE}};
    }
    TRI_ASSERT(block != nullptr || state == ExecutionState::DONE);

    if (block != nullptr) {
      // we need to store the block for later creation of AqlItemBlockInputMatrix
      _aqlItemMatrix->addBlock(std::move(block));
    }

    // If we find a ShadowRow or ExecutionState == Done, we're done fetching.
    if (_aqlItemMatrix->stoppedOnShadowRow() || state == ExecutionState::DONE) {
      if (state == ExecutionState::HASMORE) {
        return {state, skipped,
                AqlItemBlockInputMatrix{ExecutorState::HASMORE, _aqlItemMatrix.get()}};
      }
      return {state, skipped,
              AqlItemBlockInputMatrix{ExecutorState::DONE, _aqlItemMatrix.get()}};
    }
  }
}

AllRowsFetcher::AllRowsFetcher(DependencyProxy<BlockPassthrough::Disable>& executionBlock)
    : _dependencyProxy(&executionBlock),
      _aqlItemMatrix(nullptr),
      _upstreamState(ExecutionState::HASMORE),
      _blockToReturnNext(0) {}

RegisterCount AllRowsFetcher::getNrInputRegisters() const {
  return _dependencyProxy->getNrInputRegisters();
}

ExecutionState AllRowsFetcher::upstreamState() {
  if (_aqlItemMatrix == nullptr) {
    // We have not pulled anything yet!
    return ExecutionState::HASMORE;
  }
  if (_upstreamState == ExecutionState::WAITING) {
    return ExecutionState::WAITING;
  }
  if (_blockToReturnNext >= _aqlItemMatrix->numberOfBlocks()) {
    return ExecutionState::DONE;
  }
  return ExecutionState::HASMORE;
}