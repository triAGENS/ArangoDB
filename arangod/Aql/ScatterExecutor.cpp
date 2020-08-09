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

#include "ScatterExecutor.h"

#include "Aql/AqlCallStack.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/IdExecutor.h"
#include "Basics/Exceptions.h"

using namespace arangodb;
using namespace arangodb::aql;

ScatterExecutorInfos::ScatterExecutorInfos(std::vector<std::string> clientIds)
    : ClientsExecutorInfos(std::move(clientIds)) {}


ScatterExecutor::ClientBlockData::ClientBlockData(ExecutionEngine& engine,
                                                  ExecutionNode const* node,
                                                  RegisterInfos const& registerInfos)
    : _queue{}, _executor(nullptr), _executorHasMore{false} {
  // We only get shared ptrs to const data. so we need to copy here...
  IdExecutorInfos executorInfos(false, 0, "", false);
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

auto ScatterExecutor::ClientBlockData::clear() -> void {
  _queue.clear();
  _executorHasMore = false;
}

auto ScatterExecutor::ClientBlockData::addBlock(SharedAqlItemBlockPtr block,
                                                SkipResult skipped) -> void {
  // NOTE:
  // The given ItemBlock will be reused in all requesting blocks.
  // However, the next following block could be passthrough.
  // If it is, it will modify that data stored in block.
  // If now another client requests the same block, it is not
  // the original any more, but a modified version.
  // For Instance in calculation we assert that the place we write to
  // is empty. If another peer-calculation has written to this value
  // this assertion does not hold true anymore.
  // Hence we are required to do an indepth cloning here.
  if (block == nullptr) {
    _queue.emplace_back(block, skipped);
  } else {
    _queue.emplace_back(block->cloneDataAndMoveShadow(), skipped);
  }
}

auto ScatterExecutor::ClientBlockData::hasDataFor(AqlCall const& call) -> bool {
  return _executorHasMore || !_queue.empty();
}

auto ScatterExecutor::ClientBlockData::execute(AqlCallStack const& callStack, AqlCallList clientCall, ExecutionState upstreamState)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  TRI_ASSERT(_executor != nullptr);

  // Make sure we actually have data before you call execute
  TRI_ASSERT(hasDataFor(clientCall.peekNextCall()));
  if (!_executorHasMore) {
    auto const& [block, skipResult] = _queue.front();
    // This cast is guaranteed, we create this a couple lines above and only
    // this executor is used here.
    // Unfortunately i did not get a version compiled were i could only
    // forward declare the templates in header.
    auto casted =
        static_cast<ExecutionBlockImpl<IdExecutor<ConstFetcher>>*>(_executor.get());
    TRI_ASSERT(casted != nullptr);
    casted->injectConstantBlock(block, skipResult);
    _executorHasMore = true;
    _queue.pop_front();
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

ScatterExecutor::ScatterExecutor(Infos const&) {}

auto ScatterExecutor::distributeBlock(SharedAqlItemBlockPtr const& block, SkipResult skipped,
                                      std::unordered_map<std::string, ClientBlockData>& blockMap) const
    -> void {
  // Scatter returns every block on every client as is.
  for (auto& [id, list] : blockMap) {
    list.addBlock(block, skipped);
  }
}

ExecutionBlockImpl<ScatterExecutor>::ExecutionBlockImpl(ExecutionEngine* engine,
                                                        ScatterNode const* node,
                                                        RegisterInfos registerInfos,
                                                        ScatterExecutor::Infos&& executorInfos)
    : BlocksWithClientsImpl(engine, node, std::move(registerInfos), std::move(executorInfos)) {}
