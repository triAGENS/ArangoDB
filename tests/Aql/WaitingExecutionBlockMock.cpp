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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "WaitingExecutionBlockMock.h"

#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionState.h"
#include "Aql/QueryOptions.h"
#include "Aql/SkipResult.h"

#include "Logger/LogMacros.h"

#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::tests;
using namespace arangodb::tests::aql;

namespace {
static auto blocksToInfos(std::deque<SharedAqlItemBlockPtr> const& blocks) -> RegisterInfos {
  auto readInput = RegIdSet{};
  auto writeOutput = RegIdSet{};
  RegIdSet toClear{};
  RegIdSetStack toKeep{{}};
  RegisterId regs = 1;
  for (auto const& b : blocks) {
    if (b != nullptr) {
      // Find the first non-nullptr block
      regs = b->getNrRegs();

      break;
    }
  }
  // if non found sorry blind guess the number of registers here.
  // This can happen if you insert the data later into this Mock.
  // If you do so this register planning is of
  // for the rime being no test is showing this behavior.
  // Consider adding data first if the test fails

  for (RegisterId r = 0; r < regs; ++r) {
    toKeep.back().emplace(r);
  }
  return {readInput, writeOutput, regs, regs, toClear, toKeep};
}
}  // namespace
WaitingExecutionBlockMock::WaitingExecutionBlockMock(ExecutionEngine* engine,
                                                     ExecutionNode const* node,
                                                     std::deque<SharedAqlItemBlockPtr>&& data,
                                                     WaitingBehaviour variant,
                                                     size_t subqueryDepth)
    : ExecutionBlock(engine, node),
      _hasWaited(false),
      _variant{variant},
      _infos{::blocksToInfos(data)},
      _blockData{*engine, node, _infos} {
  SkipResult s;
  for (size_t i = 0; i < subqueryDepth; ++i) {
    s.incrementSubquery();
  }
  for (auto const& b : data) {
    if (b != nullptr) {
      TRI_ASSERT(s.nothingSkipped());
      _blockData.addBlock(b, s);
      if (b->hasShadowRows()) {
        _doesContainShadowRows = true;
      }
    }
  }

  if (!data.empty() && data.back() == nullptr) {
    // If the last block in _data is explicitly a nullptr
    // we will lie on the last row
    _shouldLieOnLastRow = true;
  }
}

std::pair<arangodb::aql::ExecutionState, arangodb::Result> WaitingExecutionBlockMock::initializeCursor(
    arangodb::aql::InputAqlItemRow const& input) {
  if (!_hasWaited) {
    _hasWaited = true;
    return {ExecutionState::WAITING, TRI_ERROR_NO_ERROR};
  }
  _hasWaited = false;
  return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};
}

std::pair<arangodb::aql::ExecutionState, Result> WaitingExecutionBlockMock::shutdown(int errorCode) {
  ExecutionState state;
  Result res;
  return std::make_pair(state, res);
}

std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> WaitingExecutionBlockMock::execute(AqlCallStack const& stack, arangodb::aql::AqlCallList clientCall) {
  traceExecuteBegin(stack, clientCall);
  auto res = executeWithoutTrace(stack, std::move(clientCall));
  traceExecuteEnd(res);
  return res;
}

std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> WaitingExecutionBlockMock::executeWithoutTrace(
    AqlCallStack const& stack, arangodb::aql::AqlCallList clientCall) {

  auto const& myCall = clientCall.peekNextCall();

  TRI_ASSERT(!(myCall.getOffset() == 0 && myCall.softLimit == AqlCall::Limit{0u}));
  TRI_ASSERT(!(myCall.hasSoftLimit() && myCall.fullCount));
  TRI_ASSERT(!(myCall.hasSoftLimit() && myCall.hasHardLimit()));
  if (_variant != WaitingBehaviour::NEVER && !_hasWaited) {
    // If we ordered waiting check on _hasWaited and wait if not
    _hasWaited = true;
    return {ExecutionState::WAITING, SkipResult{}, nullptr};
  }
  if (_variant == WaitingBehaviour::ALWAYS) {
    // If we always wait, reset.
    _hasWaited = false;
  }
  if (!_blockData.hasDataFor(myCall)) {
    return {ExecutionState::DONE, {}, nullptr};
  }
  SkipResult localSkipped;
  while (true) {
    auto [state, skipped, result] = _blockData.execute(stack, clientCall, ExecutionState::DONE);
    // We loop here if we only skip
    localSkipped.merge(skipped, false);
    bool shouldReturn = state == ExecutionState::DONE || result != nullptr;

    if (result != nullptr && !result->hasShadowRows()) {
      // Count produced rows
      auto& modCall = clientCall.modifyNextCall();
      modCall.didProduce(result->size());
    }

    if (!skipped.nothingSkipped()) {
      auto& modCall = clientCall.modifyNextCall();
      modCall.didSkip(skipped.getSkipCount());
      // Reset the internal counter.
      // We reuse the call to upstream
      // this inturn uses this counter to report nrRowsSkipped
      modCall.skippedRows = 0;
      if (!modCall.needSkipMore() && modCall.getLimit() == 0) {
        // We do not have anything to do for this call
        shouldReturn = true;
      }
    }
    if (shouldReturn) {
      if (!_doesContainShadowRows && state == ExecutionState::HASMORE) {
        // FullCount phase, let us loop until we are done
        // We do not have anything to do for this call
        // But let us only do this on top-level queries

        auto const& call = clientCall.peekNextCall();
        if (call.hasHardLimit() && call.getLimit() == 0) {
          // We are in fullCount/fastForward phase now.
          while (state == ExecutionState::HASMORE) {
            auto [nextState, nextSkipped, nextResult] =
                _blockData.execute(stack, clientCall, ExecutionState::DONE);
            state = nextState;
            // We are disallowed to have any result here.
            TRI_ASSERT(nextResult == nullptr);
            localSkipped.merge(nextSkipped, false);
          }
        }
      }
      // We want to "lie" on upstream if we have hit a softLimit exactly on the last row
      if (state == ExecutionState::DONE && _shouldLieOnLastRow) {
        auto const& call = clientCall.peekNextCall();
        if (call.hasSoftLimit() && call.getLimit() == 0 && call.getOffset() == 0) {
          state = ExecutionState::HASMORE;
        }
      }

      // We have a valid result.
      return {state, localSkipped, result};
    }
  }
}
