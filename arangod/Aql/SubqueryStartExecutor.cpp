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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "SubqueryStartExecutor.h"

#include "Aql/ExecutionState.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/Stats.h"

using namespace arangodb;
using namespace arangodb::aql;

SubqueryStartExecutor::SubqueryStartExecutor(Fetcher& fetcher, Infos& infos)
    : _fetcher(fetcher),
      _state(ExecutionState::HASMORE),
      _input(CreateInvalidInputRowHint{}) {}
SubqueryStartExecutor::~SubqueryStartExecutor() = default;

std::pair<ExecutionState, NoStats> SubqueryStartExecutor::produceRows(OutputAqlItemRow& output) {
  while (!output.isFull()) {
    if (_state == ExecutionState::DONE && !_input.isInitialized()) {
      // Short cut, we are done, do not fetch further
      return {ExecutionState::DONE, NoStats{}};
    }
    std::tie(_state, _input) = _fetcher.fetchRow(output.numRowsLeft() / 2);
    if (!_input.isInitialized()) {
      TRI_ASSERT(_state == ExecutionState::WAITING || _state == ExecutionState::DONE);
      return {_state, NoStats{}};
    }
    TRI_ASSERT(!output.isFull());
    output.copyRow(_input);
    output.advanceRow();
    TRI_ASSERT(!output.isFull());
    output.createShadowRow(_input);
    output.advanceRow();
    _input = InputAqlItemRow(CreateInvalidInputRowHint{});
  }

  return {ExecutionState::DONE, NoStats{}};
}