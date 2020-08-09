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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "AsyncExecutor.h"

#include "Aql/AqlValue.h"
#include "Aql/Query.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/QueryOptions.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/SharedQueryState.h"
#include "Aql/Stats.h"

#include "Logger/LogMacros.h"

#include <algorithm>
#include <utility>

using namespace arangodb;
using namespace arangodb::aql;

ExecutionBlockImpl<AsyncExecutor>::ExecutionBlockImpl(
    ExecutionEngine* engine, AsyncNode const* node)
    : ExecutionBlock(engine, node),
      _sharedState(engine->sharedState()) {}

std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> ExecutionBlockImpl<AsyncExecutor>::execute(AqlCallStack const& stack, AqlCallList clientCall) {
  traceExecuteBegin(stack, clientCall);
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

std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> ExecutionBlockImpl<AsyncExecutor>::executeWithoutTrace(AqlCallStack const& stack, AqlCallList clientCall) {
  
//  if (getQuery().killed()) {
//    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
//  }

  std::lock_guard<std::mutex> guard(_mutex);
  
  TRI_ASSERT(_dependencies.size() == 1);

  if (_internalState == AsyncState::InProgress) {
    return {ExecutionState::WAITING, SkipResult{}, SharedAqlItemBlockPtr()};
  } else if (_internalState == AsyncState::GotResult) {
    if (_returnState != ExecutionState::DONE) {
      // we may not return WAITING if upstream returned DONE
      _internalState = AsyncState::Empty;
    }
    return {_returnState, std::move(_returnSkip), std::move(_returnBlock)};
  } else if (_internalState == AsyncState::GotException) {
    TRI_ASSERT(_returnException != nullptr);
    std::rethrow_exception(_returnException);
    TRI_ASSERT(false);
    return {ExecutionState::DONE, SkipResult(), SharedAqlItemBlockPtr()};
  }
  TRI_ASSERT(_internalState == AsyncState::Empty);

  _internalState = AsyncState::InProgress;
  bool queued = _sharedState->asyncExecuteAndWakeup([this, stack, clientCall](bool isAsync) {
    std::unique_lock<std::mutex> guard(_mutex, std::defer_lock);

    try {
      auto [state, skip, block] = _dependencies[0]->execute(stack, clientCall);
      if (isAsync) {
        guard.lock();
      }
      _returnState = state;
      _returnSkip = std::move(skip);
      _returnBlock = std::move(block);
      
      _internalState = AsyncState::GotResult;
    } catch(...) {
      if (isAsync) {
        guard.lock();
      }
      _returnException = std::current_exception();
      _internalState = AsyncState::GotException;
    }
  });

  if (!queued) {
    _internalState = AsyncState::Empty;
    return {_returnState, std::move(_returnSkip), std::move(_returnBlock)};
  }
  return {ExecutionState::WAITING, SkipResult{}, SharedAqlItemBlockPtr()};
}

std::pair<ExecutionState, Result> ExecutionBlockImpl<AsyncExecutor>::initializeCursor(
    InputAqlItemRow const& input) {
//
//  if (getQuery().killed()) {
//    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
//  }
  
  TRI_ASSERT(_dependencies.size() == 1);
  
  std::lock_guard<std::mutex> guard(_mutex);
  auto res = ExecutionBlock::initializeCursor(input);
  _returnBlock = nullptr;
  _returnState = ExecutionState::HASMORE;
  _internalState = AsyncState::Empty;
  return res;
}

/// @brief shutdown, will be called exactly once for the whole query
std::pair<ExecutionState, Result> ExecutionBlockImpl<AsyncExecutor>::shutdown(int errorCode) {
  std::lock_guard<std::mutex> guard(_mutex);
  auto res = ExecutionBlock::shutdown(errorCode);
  _returnBlock = nullptr;
  _returnState = ExecutionState::HASMORE;
  _internalState = AsyncState::Empty;
  return res;
}
