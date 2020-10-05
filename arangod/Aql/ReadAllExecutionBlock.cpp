////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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

#include "ReadAllExecutionBlock.h"

#include "Aql/AqlCallStack.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/QueryContext.h"
#include "Basics/Exceptions.h"

using namespace arangodb::aql;

ReadAllExecutionBlock::ReadAllExecutionBlock(ExecutionEngine* engine,
                                             ExecutionNode const* en, RegisterInfos registerInfos)
    : ExecutionBlock(engine, en),
      _query(engine->getQuery()),
      _clientBlockData(*engine, en, registerInfos) {}

ReadAllExecutionBlock::~ReadAllExecutionBlock() = default;

[[nodiscard]] QueryContext const& ReadAllExecutionBlock::getQuery() const {
  return _query;
}

std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> ReadAllExecutionBlock::execute(AqlCallStack stack) {
  if (getQuery().killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
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
  auto res = executeWithoutTrace(std::move(stack));
  traceExecuteEnd(res);
  return res;
}

auto ReadAllExecutionBlock::executeWithoutTrace(AqlCallStack stack)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  // This block only works with a single dependency
  TRI_ASSERT(_dependencies.size() == 1);

  // This block fetches until the upstream returns done, before it returns anything
  while (_upstreamState != ExecutionState::DONE) {
    auto [state, skipResult, block] =
        _dependencies.at(0)->execute(stack.createEquivalentFetchAllShadowRowsStack());
    if (state == ExecutionState::WAITING) {
      return {state, SkipResult{}, nullptr};
    }
    _upstreamState = state;
    // We can never skip anything here.
    TRI_ASSERT(skipResult.nothingSkipped());
    _clientBlockData.addBlock(block, skipResult);
  }
  // We need to have at least the skip information once, otherwise we call here although we returned done.
  TRI_ASSERT(_clientBlockData.hasDataFor(stack.peek()));
  // We can never skip anything here.
  if (_clientBlockData.hasDataFor(stack.peek())) {
    return _clientBlockData.execute(stack, _upstreamState);
  }
  // Just emergency bailout.
  return {ExecutionState::DONE, SkipResult{}, nullptr};
}
