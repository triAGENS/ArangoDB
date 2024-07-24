////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "EnumerateListExecutor.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlItemBlockInputRange.h"
#include "Aql/AqlValue.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/QueryContext.h"
#include "Aql/RegisterInfos.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/Stats.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace {
void throwArrayExpectedException(AqlValue const& value) {
  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_QUERY_ARRAY_EXPECTED,
      StringUtils::concatT(
          "collection or ", TRI_errno_string(TRI_ERROR_QUERY_ARRAY_EXPECTED),
          " as operand to FOR loop; you provided a value of type '",
          value.getTypeString(), "'"));
}
}  // namespace

EnumerateListExecutorInfos::EnumerateListExecutorInfos(
    QueryContext& query, RegisterId inputRegister, RegisterId outputRegister)
    : _query(query),
      _inputRegister(inputRegister),
      _outputRegister(outputRegister) {}

RegisterId EnumerateListExecutorInfos::getInputRegister() const noexcept {
  return _inputRegister;
}

RegisterId EnumerateListExecutorInfos::getOutputRegister() const noexcept {
  return _outputRegister;
}

QueryContext& EnumerateListExecutorInfos::getQuery() const noexcept {
  return _query;
}

EnumerateListExecutor::EnumerateListExecutor(Fetcher& fetcher,
                                             EnumerateListExecutorInfos& infos)
    : _infos(infos),
      _currentRow{CreateInvalidInputRowHint{}},
      _inputArrayPosition(0),
      _inputArrayLength(0) {}

void EnumerateListExecutor::initializeNewRow(
    AqlItemBlockInputRange& inputRange) {
  if (_currentRow) {
    inputRange.advanceDataRow();
  }
  std::tie(_currentRowState, _currentRow) = inputRange.peekDataRow();
  if (!_currentRow) {
    return;
  }

  // fetch new row, put it in local state
  AqlValue const& inputList = _currentRow.getValue(_infos.getInputRegister());

  // store the length into a local variable
  // so we don't need to calculate length every time
  if (!inputList.isArray()) {
    throwArrayExpectedException(inputList);
  }
  _inputArrayLength = inputList.length();

  _inputArrayPosition = 0;
}

void EnumerateListExecutor::processArrayElement(OutputAqlItemRow& output) {
  bool mustDestroy;
  AqlValue const& inputList = _currentRow.getValue(_infos.getInputRegister());
  AqlValue innerValue =
      getAqlValue(inputList, _inputArrayPosition, mustDestroy);
  AqlValueGuard guard(innerValue, mustDestroy);

  TRI_IF_FAILURE("EnumerateListBlock::getSome") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  output.moveValueInto(_infos.getOutputRegister(), _currentRow, guard);
  output.advanceRow();

  // set position to +1 for next iteration
  _inputArrayPosition++;
}

size_t EnumerateListExecutor::skipArrayElement(size_t toSkip) {
  size_t skipped = 0;

  if (toSkip <= _inputArrayLength - _inputArrayPosition) {
    // if we're skipping less or exact the amount of elements we can skip with
    // toSkip
    _inputArrayPosition += toSkip;
    skipped = toSkip;
  } else if (toSkip > _inputArrayLength - _inputArrayPosition) {
    // we can only skip the max amount of values we've in our array
    skipped = _inputArrayLength - _inputArrayPosition;
    _inputArrayPosition = _inputArrayLength;
  }
  return skipped;
}

std::tuple<ExecutorState, NoStats, AqlCall> EnumerateListExecutor::produceRows(
    AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output) {
  AqlCall upstreamCall{};
  upstreamCall.fullCount = output.getClientCall().fullCount;

  while (inputRange.hasDataRow() && !output.isFull()) {
    if (_inputArrayLength == _inputArrayPosition) {
      // we reached either the end of an array
      // or are in our first loop iteration
      initializeNewRow(inputRange);
      continue;
    }

    TRI_ASSERT(_inputArrayPosition < _inputArrayLength);
    processArrayElement(output);

    _killCheckCounter = (_killCheckCounter + 1) % 1024;
    if (ADB_UNLIKELY(_killCheckCounter == 0 && _infos.getQuery().killed())) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    }
  }

  if (_inputArrayLength == _inputArrayPosition) {
    // we reached either the end of an array
    // or are in our first loop iteration
    initializeNewRow(inputRange);
  }

  return {inputRange.upstreamState(), NoStats{}, upstreamCall};
}

std::tuple<ExecutorState, NoStats, size_t, AqlCall>
EnumerateListExecutor::skipRowsRange(AqlItemBlockInputRange& inputRange,
                                     AqlCall& call) {
  InputAqlItemRow input{CreateInvalidInputRowHint{}};

  while (inputRange.hasDataRow() && call.shouldSkip()) {
    if (_inputArrayLength == _inputArrayPosition) {
      // we reached either the end of an array
      // or are in our first loop iteration
      initializeNewRow(inputRange);
      continue;
    }

    TRI_ASSERT(_inputArrayPosition < _inputArrayLength);

    auto const skip = std::invoke([&] {
      // if offset is > 0, we're in offset skip phase
      if (call.getOffset() > 0) {
        // we still need to skip offset entries
        return call.getOffset();
      } else {
        TRI_ASSERT(call.needsFullCount());
        // fullCount phase - skippen bis zum ende
        return _inputArrayLength - _inputArrayPosition;
      }
    });
    auto const skipped = skipArrayElement(skip);
    call.didSkip(skipped);

    _killCheckCounter = (_killCheckCounter + 1) % 1024;
    if (ADB_UNLIKELY(_killCheckCounter == 0 && _infos.getQuery().killed())) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    }
  }

  if (_inputArrayPosition < _inputArrayLength) {
    // fullCount will always skip the complete array
    return {ExecutorState::HASMORE, NoStats{}, call.getSkipCount(), AqlCall{}};
  }
  return {inputRange.upstreamState(), NoStats{}, call.getSkipCount(),
          AqlCall{}};
}

void EnumerateListExecutor::initialize() {
  _inputArrayLength = 0;
  _inputArrayPosition = 0;
  _currentRow = InputAqlItemRow{CreateInvalidInputRowHint{}};
}

/// @brief create an AqlValue from the inVariable using the current _index
AqlValue EnumerateListExecutor::getAqlValue(AqlValue const& inVarReg,
                                            size_t const& pos,
                                            bool& mustDestroy) {
  TRI_IF_FAILURE("EnumerateListBlock::getAqlValue") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return inVarReg.at(pos, mustDestroy, true);
}
