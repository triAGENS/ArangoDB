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
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "SingleRowFetcher.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/DependencyProxy.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/SkipResult.h"

using namespace arangodb;
using namespace arangodb::aql;

template <BlockPassthrough passBlocksThrough>
SingleRowFetcher<passBlocksThrough>::SingleRowFetcher(DependencyProxy<passBlocksThrough>& executionBlock)
    : _dependencyProxy(&executionBlock){}

template <BlockPassthrough passBlocksThrough>
SingleRowFetcher<passBlocksThrough>::SingleRowFetcher()
    : _dependencyProxy(nullptr) {}

template <BlockPassthrough passBlocksThrough>
std::tuple<ExecutionState, SkipResult, AqlItemBlockInputRange>
SingleRowFetcher<passBlocksThrough>::execute(AqlCallStack const& stack, AqlCallList clientCall) {
  auto [state, skipped, block] = _dependencyProxy->execute(stack, std::move(clientCall));
  if (state == ExecutionState::WAITING) {
    // On waiting we have nothing to return
    return {state, SkipResult{}, AqlItemBlockInputRange{ExecutorState::HASMORE}};
  }
  if (block == nullptr) {
    if (state == ExecutionState::HASMORE) {
      return {state, skipped,
              AqlItemBlockInputRange{ExecutorState::HASMORE, skipped.getSkipCount()}};
    }
    return {state, skipped,
            AqlItemBlockInputRange{ExecutorState::DONE, skipped.getSkipCount()}};
  }

  TRI_ASSERT(block != nullptr);
  auto [start, end] = block->getRelevantRange();
  if (state == ExecutionState::HASMORE) {
    return {state, skipped,
            AqlItemBlockInputRange{ExecutorState::HASMORE,
                                   skipped.getSkipCount(), block, start}};
  }
  return {state, skipped,
          AqlItemBlockInputRange{ExecutorState::DONE, skipped.getSkipCount(), block, start}};
}



template <BlockPassthrough blockPassthrough>
void SingleRowFetcher<blockPassthrough>::setDistributeId(std::string const& id) {
  _dependencyProxy->setDistributeId(id);
}

template class ::arangodb::aql::SingleRowFetcher<BlockPassthrough::Disable>;
template class ::arangodb::aql::SingleRowFetcher<BlockPassthrough::Enable>;
