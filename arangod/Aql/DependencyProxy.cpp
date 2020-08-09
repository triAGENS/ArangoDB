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

#include "DependencyProxy.h"

#include "Aql/AqlCallStack.h"
#include "Aql/BlocksWithClients.h"
#include "Aql/types.h"
#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"

using namespace arangodb;
using namespace arangodb::aql;

template <BlockPassthrough blockPassthrough>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
DependencyProxy<blockPassthrough>::execute(AqlCallStack const& stack, AqlCallList clientCall) {
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped;
  SharedAqlItemBlockPtr block = nullptr;
  // Note: upstreamBlock will return next dependency
  // if we need to loop here
  do {
    // TODO:MCHACKI need to copy here as we do not know when advance will kick in
    std::tie(state, skipped, block) = executeForDependency(_currentDependency, stack, clientCall);

    if (state == ExecutionState::DONE) {
      if (!advanceDependency()) {
        break;
      }
    }
  } while (state != ExecutionState::WAITING && skipped.nothingSkipped() && block == nullptr);
  return {state, skipped, std::move(block)};
}

template <BlockPassthrough blockPassthrough>
std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>
DependencyProxy<blockPassthrough>::executeForDependency(size_t dependency,
                                                        AqlCallStack const& stack,
                                                        AqlCallList clientCall) {
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped;
  SharedAqlItemBlockPtr block = nullptr;

  if (!_distributeId.empty()) {
    // We are in the cluster case.
    // we have to ask executeForShard
    auto upstreamWithClient =
        dynamic_cast<BlocksWithClients*>(&upstreamBlockForDependency(dependency));
    TRI_ASSERT(upstreamWithClient != nullptr);
    if (upstreamWithClient == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL_AQL,
                                     "Invalid state reached, we try to "
                                     "request sharded data from a block "
                                     "that is not able to provide it.");
    }
    std::tie(state, skipped, block) =
        upstreamWithClient->executeForClient(stack, std::move(clientCall), _distributeId);
  } else {
    std::tie(state, skipped, block) =
        upstreamBlockForDependency(dependency).execute(stack, std::move(clientCall));
  }
  TRI_IF_FAILURE("ExecutionBlock::getBlock") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (skipped.nothingSkipped() && block == nullptr) {
    // We're either waiting or Done
    TRI_ASSERT(state == ExecutionState::DONE || state == ExecutionState::WAITING);
  }
  return {state, skipped, std::move(block)};
}

template <BlockPassthrough blockPassthrough>
DependencyProxy<blockPassthrough>::DependencyProxy(
    std::vector<ExecutionBlock*> const& dependencies,
    RegisterCount nrInputRegisters)
    : _dependencies(dependencies),
      _nrInputRegisters(nrInputRegisters),
      _distributeId(),
      _currentDependency(0) {
}

template <BlockPassthrough blockPassthrough>
RegisterCount DependencyProxy<blockPassthrough>::getNrInputRegisters() const {
  return _nrInputRegisters;
}

template <BlockPassthrough blockPassthrough>
size_t DependencyProxy<blockPassthrough>::numberDependencies() const {
  return _dependencies.size();
}

template <BlockPassthrough blockPassthrough>
void DependencyProxy<blockPassthrough>::reset() {
  _currentDependency = 0;
}

template <BlockPassthrough blockPassthrough>
ExecutionBlock& DependencyProxy<blockPassthrough>::upstreamBlock() {
  return upstreamBlockForDependency(_currentDependency);
}

template <BlockPassthrough blockPassthrough>
ExecutionBlock& DependencyProxy<blockPassthrough>::upstreamBlockForDependency(size_t index) {
  TRI_ASSERT(_dependencies.size() > index);
  return *_dependencies[index];
}

template <BlockPassthrough blockPassthrough>
bool DependencyProxy<blockPassthrough>::advanceDependency() {
  if (_currentDependency + 1 >= _dependencies.size()) {
    return false;
  }
  _currentDependency++;
  return true;
}

template class ::arangodb::aql::DependencyProxy<BlockPassthrough::Enable>;
template class ::arangodb::aql::DependencyProxy<BlockPassthrough::Disable>;
