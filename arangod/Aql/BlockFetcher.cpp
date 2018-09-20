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

#include "BlockFetcher.h"

std::pair<arangodb::aql::ExecutionState,
          std::shared_ptr<arangodb::aql::AqlItemBlockShell>>
arangodb::aql::BlockFetcher::fetchBlock() {
  ExecutionState state;
  std::unique_ptr<AqlItemBlock> block;
  std::tie(state, block) = _executionBlock->fetchBlock();
  if (block != nullptr) {
    _blockId++;
    // no output registers for input blocks
    auto outputRegisters = std::make_shared<std::unordered_set<RegisterId>>();
    auto shell = std::make_shared<AqlItemBlockShell>(
        _executionBlock->_engine->_itemBlockManager, std::move(block),
        _inputRegisters, outputRegisters, _blockId);
    return {state, shell};
  } else {
    return {state, nullptr};
  }
}
