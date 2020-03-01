////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "BlocksWithClients.h"

#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlTransaction.h"
#include "Aql/AqlValue.h"
#include "Aql/BlockCollector.h"
#include "Aql/Collection.h"
#include "Aql/DistributeExecutor.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionStats.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/ScatterExecutor.h"
#include "Aql/SkipResult.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Scheduler/SchedulerFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

using VelocyPackHelper = arangodb::basics::VelocyPackHelper;
using StringBuffer = arangodb::basics::StringBuffer;

ClientsExecutorInfos::ClientsExecutorInfos(std::vector<std::string> clientIds)
    : _clientIds(std::move(clientIds)) {
  TRI_ASSERT(!_clientIds.empty());
};

auto ClientsExecutorInfos::nrClients() const noexcept -> size_t {
  return _clientIds.size();
}
auto ClientsExecutorInfos::clientIds() const noexcept -> std::vector<std::string> const& {
  return _clientIds;
}

template <class Executor>
BlocksWithClientsImpl<Executor>::BlocksWithClientsImpl(ExecutionEngine* engine,
                                                       ExecutionNode const* ep,
                                                       typename Executor::Infos infos)
    : ExecutionBlock(engine, ep),
      BlocksWithClients(),
      _nrClients(infos.nrClients()),
      _type(ScatterNode::ScatterType::SHARD),
      _infos(std::move(infos)),
      _executor{_infos},
      _clientBlockData{},
      _wasShutdown(false) {
  _shardIdMap.reserve(_nrClients);
  auto const& shardIds = _infos.clientIds();
  for (size_t i = 0; i < _nrClients; i++) {
    _shardIdMap.try_emplace(shardIds[i], i);
  }

  auto scatter = ExecutionNode::castTo<ScatterNode const*>(ep);
  TRI_ASSERT(scatter != nullptr);
  _type = scatter->getScatterType();

  _clientBlockData.reserve(shardIds.size());

  auto readAble = make_shared_unordered_set();
  auto writeAble = make_shared_unordered_set();

  for (auto const& id : shardIds) {
    _clientBlockData.try_emplace(id, typename Executor::ClientBlockData{*engine, scatter, _infos});
  }
}

/// @brief initializeCursor
template <class Executor>
auto BlocksWithClientsImpl<Executor>::initializeCursor(InputAqlItemRow const& input)
    -> std::pair<ExecutionState, Result> {
  for (auto& [key, list] : _clientBlockData) {
    list.clear();
  }
  return ExecutionBlock::initializeCursor(input);
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::getBlock(size_t atMost)
    -> std::pair<ExecutionState, bool> {
  if (_engine->getQuery()->killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  auto res = _dependencies[0]->getSome(atMost);
  if (res.first == ExecutionState::WAITING) {
    return {res.first, false};
  }

  TRI_IF_FAILURE("ExecutionBlock::getBlock") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  _upstreamState = res.first;

  if (res.second != nullptr) {
    _buffer.emplace_back(std::move(res.second));
    return {res.first, true};
  }

  return {res.first, false};
}

/// @brief shutdown
template <class Executor>
auto BlocksWithClientsImpl<Executor>::shutdown(int errorCode)
    -> std::pair<ExecutionState, Result> {
  if (_wasShutdown) {
    return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};
  }
  auto res = ExecutionBlock::shutdown(errorCode);
  if (res.first == ExecutionState::WAITING) {
    return res;
  }
  _wasShutdown = true;
  return res;
}

/// @brief getClientId: get the number <clientId> (used internally)
/// corresponding to <shardId>
template <class Executor>
size_t BlocksWithClientsImpl<Executor>::getClientId(std::string const& shardId) const {
  if (shardId.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "got empty distribution id");
  }
  auto it = _shardIdMap.find(shardId);
  if (it == _shardIdMap.end()) {
    std::string message("AQL: unknown distribution id ");
    message.append(shardId);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, message);
  }
  return it->second;
}

template <class Executor>
std::pair<ExecutionState, SharedAqlItemBlockPtr> BlocksWithClientsImpl<Executor>::getSome(size_t) {
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

template <class Executor>
std::pair<ExecutionState, size_t> BlocksWithClientsImpl<Executor>::skipSome(size_t) {
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

template <class Executor>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
BlocksWithClientsImpl<Executor>::execute(AqlCallStack stack) {
  // This will not be implemented here!
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::executeForClient(AqlCallStack stack,
                                                       std::string const& clientId)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  // traceExecuteBegin(stack);
  auto res = executeWithoutTraceForClient(stack, clientId);
  // traceExecuteEnd(res);
  return res;
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::executeWithoutTraceForClient(AqlCallStack stack,
                                                                   std::string const& clientId)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  TRI_ASSERT(!clientId.empty());
  if (clientId.empty()) {
    // Security bailout to avoid UB
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "got empty distribution id");
  }

  auto it = _clientBlockData.find(clientId);
  TRI_ASSERT(it != _clientBlockData.end());
  if (it == _clientBlockData.end()) {
    // Security bailout to avoid UB
    std::string message("AQL: unknown distribution id ");
    message.append(clientId);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, message);
  }

  // This call is only used internally.
  auto call = stack.isRelevant() ? stack.popCall() : AqlCall{};

  // We do not have anymore data locally.
  // Need to fetch more from upstream
  auto& dataContainer = it->second;

  while (!dataContainer.hasDataFor(call)) {
    if (_upstreamState == ExecutionState::DONE) {
      // We are done, with everything, we will not be able to fetch any more rows
      return {_upstreamState, SkipResult{}, nullptr};
    }

    auto state = fetchMore(stack);
    if (state == ExecutionState::WAITING) {
      return {state, SkipResult{}, nullptr};
    }
    _upstreamState = state;
  }
  // If we get here we have data and can return it.
  return dataContainer.execute(call, _upstreamState);
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::fetchMore(AqlCallStack stack) -> ExecutionState {
  if (_engine->getQuery()->killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  // NOTE: We do not handle limits / skip here
  // They can differ between different calls to this executor.
  // We may need to revisit this for performance reasons.
  AqlCall call{};
  stack.pushCall(std::move(call));

  TRI_ASSERT(_dependencies.size() == 1);
  auto [state, skipped, block] = _dependencies[0]->execute(stack);

  // We can never ever forward skip!
  // We could need the row in a different block, and once skipped
  // we cannot get it back.
  TRI_ASSERT(skipped.nothingSkipped());

  TRI_IF_FAILURE("ExecutionBlock::getBlock") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // Waiting -> no block
  TRI_ASSERT(state != ExecutionState::WAITING || block == nullptr);
  if (block != nullptr) {
    _executor.distributeBlock(block, _clientBlockData);
  }

  return state;
}

/// @brief getSomeForShard
/// @deprecated
template <class Executor>
std::pair<ExecutionState, SharedAqlItemBlockPtr> BlocksWithClientsImpl<Executor>::getSomeForShard(
    size_t atMost, std::string const& shardId) {
  AqlCallStack stack(AqlCall::SimulateGetSome(atMost), true);
  auto [state, skipped, block] = executeForClient(stack, shardId);
  TRI_ASSERT(skipped.nothingSkipped());
  return {state, block};
}

/// @brief skipSomeForShard
/// @deprecated
template <class Executor>
std::pair<ExecutionState, size_t> BlocksWithClientsImpl<Executor>::skipSomeForShard(
    size_t atMost, std::string const& shardId) {
  AqlCallStack stack(AqlCall::SimulateSkipSome(atMost), true);
  auto [state, skipped, block] = executeForClient(stack, shardId);
  TRI_ASSERT(block == nullptr);
  return {state, skipped.getSkipCount()};
}

template class ::arangodb::aql::BlocksWithClientsImpl<ScatterExecutor>;
template class ::arangodb::aql::BlocksWithClientsImpl<DistributeExecutor>;