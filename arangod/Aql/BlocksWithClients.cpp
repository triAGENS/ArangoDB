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
#include "Aql/Collection.h"
#include "Aql/DistributeExecutor.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionStats.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/MutexExecutor.h"
#include "Aql/MutexNode.h"
#include "Aql/Query.h"
#include "Aql/ScatterExecutor.h"
#include "Aql/SkipResult.h"
#include "Basics/Exceptions.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
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
}

auto ClientsExecutorInfos::nrClients() const noexcept -> size_t {
  return _clientIds.size();
}

auto ClientsExecutorInfos::clientIds() const noexcept -> std::vector<std::string> const& {
  return _clientIds;
}

template <class Executor>
BlocksWithClientsImpl<Executor>::BlocksWithClientsImpl(ExecutionEngine* engine,
                                                       ExecutionNode const* ep,
                                                       RegisterInfos registerInfos,
                                                       typename Executor::Infos executorInfos)
    : ExecutionBlock(engine, ep),
      BlocksWithClients(),
      _nrClients(executorInfos.nrClients()),
      _type(ScatterNode::ScatterType::SHARD),
      _registerInfos(std::move(registerInfos)),
      _executorInfos(std::move(executorInfos)),
      _executor{_executorInfos},
      _clientBlockData{},
      _wasShutdown(false) {
  _shardIdMap.reserve(_nrClients);
  auto const& shardIds = _executorInfos.clientIds();
  for (size_t i = 0; i < _nrClients; i++) {
    _shardIdMap.try_emplace(shardIds[i], i);
  }
        
  _clientBlockData.reserve(shardIds.size());
        
  if constexpr (std::is_same<MutexExecutor, Executor>::value) {
    auto* mutex = ExecutionNode::castTo<MutexNode const*>(ep);
    TRI_ASSERT(mutex != nullptr);

    for (auto const& id : shardIds) {
      _clientBlockData.try_emplace(id, typename Executor::ClientBlockData{*engine, mutex, _registerInfos});
    }
  } else {
    auto* scatter = ExecutionNode::castTo<ScatterNode const*>(ep);
    TRI_ASSERT(scatter != nullptr);
    _type = scatter->getScatterType();

    for (auto const& id : shardIds) {
      _clientBlockData.try_emplace(id, typename Executor::ClientBlockData{*engine, scatter, _registerInfos});
    }
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
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
BlocksWithClientsImpl<Executor>::execute(AqlCallStack const& stack, AqlCallList) {
  // This will not be implemented here!
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::executeForClient(AqlCallStack const& stack,
                                                       AqlCallList clientCall,
                                                       std::string const& clientId)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  if constexpr (std::is_same<MutexExecutor, Executor>::value) {
    _executor.acquireLock();
  }
  
  auto guard = scopeGuard([this]() {
    if constexpr (std::is_same<MutexExecutor, Executor>::value) {
      _executor.releaseLock();
    } else {
      // mark "this" as unused. unfortunately we cannot use [[maybe_unsed]]
      // in the lambda capture as it does not parse
      (void) this;
    }
  });
  
  traceExecuteBegin(stack, clientCall, clientId);
  auto res = executeWithoutTraceForClient(stack, std::move(clientCall), clientId);
  traceExecuteEnd(res, clientId);
  return res;
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::executeWithoutTraceForClient(
    AqlCallStack const& stack, AqlCallList clientCall, std::string const& clientId)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  TRI_ASSERT(!clientId.empty());
  if (ADB_UNLIKELY(clientId.empty())) {
    // Security bailout to avoid UB
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "got empty distribution id");
  }

  auto it = _clientBlockData.find(clientId);
  TRI_ASSERT(it != _clientBlockData.end());
  if (ADB_UNLIKELY(it == _clientBlockData.end())) {
    // Security bailout to avoid UB
    std::string message("AQL: unknown distribution id ");
    message.append(clientId);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, message);
  }

  // This call is only used internally.
  auto call = clientCall.peekNextCall();

  // We do not have anymore data locally.
  // Need to fetch more from upstream
  auto& dataContainer = it->second;
  while (true) {
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
    {
      // If we get here we have data and can return it.
      // However the call might force us to drop everything (e.g. hardLimit ==
      // 0) So we need to refetch data eventually.
      auto [state, skipped, result] = dataContainer.execute(stack, clientCall, _upstreamState);
      if (state == ExecutionState::DONE || !skipped.nothingSkipped() || result != nullptr) {
        // We have a valid result.
        return {state, skipped, std::move(result)};
      }
    }
  }
}

template <class Executor>
auto BlocksWithClientsImpl<Executor>::fetchMore(AqlCallStack const& stack) -> ExecutionState {
  if (_engine->getQuery().killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  TRI_ASSERT(_dependencies.size() == 1);
  // NOTE: We do not handle limits / skip here
  // They can differ between different calls to this executor.
  // We may need to revisit this for performance reasons.
  auto [state, skipped, block] = _dependencies[0]->execute(stack, AqlCallList{AqlCall{}, AqlCall{}});

  // We could need the row in a different block, and once skipped
  // we cannot get it back.
  TRI_ASSERT(skipped.getSkipCount() == 0);

  TRI_IF_FAILURE("ExecutionBlock::getBlock") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // Waiting -> no block
  TRI_ASSERT(state != ExecutionState::WAITING || block == nullptr);

  if (state != ExecutionState::WAITING) {
    // We need to report everything that is not waiting
    _executor.distributeBlock(block, skipped, _clientBlockData);
  }

  return state;
}

/// @brief getSomeForShard
/// @deprecated
template <class Executor>
std::pair<ExecutionState, SharedAqlItemBlockPtr> BlocksWithClientsImpl<Executor>::getSomeForShard(
    size_t atMost, std::string const& shardId) {
        AqlCallStack stack(true);
  AqlCallList clientCall{AqlCall::SimulateGetSome(atMost)}; 
  auto [state, skipped, block] = executeForClient(stack, std::move(clientCall), shardId);
  TRI_ASSERT(skipped.nothingSkipped());
  return {state, std::move(block)};
}

/// @brief skipSomeForShard
/// @deprecated
template <class Executor>
std::pair<ExecutionState, size_t> BlocksWithClientsImpl<Executor>::skipSomeForShard(
    size_t atMost, std::string const& shardId) {
  AqlCallStack stack(true);
  AqlCallList clientCall{AqlCall::SimulateSkipSome(atMost)}; 
  auto [state, skipped, block] = executeForClient(stack, std::move(clientCall), shardId);
  TRI_ASSERT(block == nullptr);
  return {state, skipped.getSkipCount()};
}

template class ::arangodb::aql::BlocksWithClientsImpl<ScatterExecutor>;
template class ::arangodb::aql::BlocksWithClientsImpl<DistributeExecutor>;
template class ::arangodb::aql::BlocksWithClientsImpl<MutexExecutor>;
