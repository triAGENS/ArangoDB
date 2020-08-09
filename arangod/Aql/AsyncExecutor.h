////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE2.0
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

#ifndef ARANGOD_AQL_ASYNC_EXECUTOR_H
#define ARANGOD_AQL_ASYNC_EXECUTOR_H

#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionState.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/SingleRowFetcher.h"

#include <mutex>
#include <tuple>
#include <utility>

namespace arangodb {
namespace aql {

class AsyncNode;
class NoStats;
class OutputAqlItemRow;
class SharedQueryState;


// The RemoteBlock is actually implemented by specializing ExecutionBlockImpl,
// so this class only exists to identify the specialization.
class AsyncExecutor final {};

/**
 * @brief See ExecutionBlockImpl.h for documentation.
 */
template <>
class ExecutionBlockImpl<AsyncExecutor> : public ExecutionBlock {
 public:
  // TODO Even if it's not strictly necessary here, for consistency's sake the
  // nonstandard arguments (server, ownName and queryId) should probably be
  // moved into some AsyncExecutorInfos class.
  ExecutionBlockImpl(ExecutionEngine* engine, AsyncNode const* node);

  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(AqlCallStack const& stack, AqlCallList clientCall) override;
 
  std::pair<ExecutionState, Result> initializeCursor(InputAqlItemRow const& input) override;

  std::pair<ExecutionState, Result> shutdown(int errorCode) override;

 private:
  
  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> executeWithoutTrace(AqlCallStack const& stack, AqlCallList clientCall);
  
  enum class AsyncState {
    Empty,
    InProgress,
    GotResult,
    GotException
  };

 private:

  std::shared_ptr<SharedQueryState> _sharedState;

  std::mutex _mutex;
  SkipResult _returnSkip;
  SharedAqlItemBlockPtr _returnBlock;
  std::exception_ptr _returnException;
  
  ExecutionState _returnState = ExecutionState::HASMORE;
  AsyncState _internalState = AsyncState::Empty;
};

}  // namespace aql
}  // namespace arangodb

#endif
