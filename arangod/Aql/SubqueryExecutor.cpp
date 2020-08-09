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

#include "SubqueryExecutor.h"

#include "Aql/AqlCallStack.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionNode.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Logger/LogMacros.h"

#define INTERNAL_LOG_SQ LOG_DEVEL_IF(false)

using namespace arangodb;
using namespace arangodb::aql;

SubqueryExecutorInfos::SubqueryExecutorInfos(ExecutionBlock& subQuery,
                                             RegisterId outReg, bool subqueryIsConst)
    : _subQuery(subQuery),
      _outReg(outReg),
      _returnsData(subQuery.getPlanNode()->getType() == ExecutionNode::RETURN),
      _isConst(subqueryIsConst) {}

template <bool isModificationSubquery>
SubqueryExecutor<isModificationSubquery>::SubqueryExecutor(Fetcher& fetcher,
                                                           SubqueryExecutorInfos& infos)
    : _fetcher(fetcher),
      _infos(infos),
      _state(ExecutorState::HASMORE),
      _subqueryInitialized(false),
      _shutdownDone(false),
      _shutdownResult(TRI_ERROR_INTERNAL),
      _subquery(infos.getSubquery()),
      _subqueryResults(nullptr),
      _input(CreateInvalidInputRowHint{}) {}

template <bool isModificationSubquery>
auto SubqueryExecutor<isModificationSubquery>::initializeSubquery(AqlItemBlockInputRange& input)
    -> std::tuple<ExecutionState, bool> {
  // init new subquery
  if (!_input) {
    std::tie(_state, _input) = input.nextDataRow();
    INTERNAL_LOG_SQ << uint64_t(this) << " nextDataRow: " << _state << " "
                 << _input.isInitialized();
    if (!_input) {
      INTERNAL_LOG_SQ << uint64_t(this) << "exit, no more input" << _state;
      return {translatedReturnType(), false};
    }
  }

  TRI_ASSERT(_input);
  if (!_infos.isConst() || _input.isFirstDataRowInBlock()) {
    INTERNAL_LOG_SQ << "Subquery: Initialize cursor";
    auto [state, result] = _subquery.initializeCursor(_input);
    if (state == ExecutionState::WAITING) {
      INTERNAL_LOG_SQ << "Waiting on initialize cursor";
      return {state, false};
    }

    if (result.fail()) {
      // Error during initialize cursor
      THROW_ARANGO_EXCEPTION(result);
    }
    _subqueryResults = std::make_unique<std::vector<SharedAqlItemBlockPtr>>();
  }
  // on const subquery we can retoggle init as soon as we have new input.
  _subqueryInitialized = true;
  return {translatedReturnType(), true};
}

/**
 * This follows the following state machine:
 * If we have a subquery ongoing we need to ask it for hasMore, until it is DONE.
 * In the case of DONE we write the result, and remove it from ongoing.
 * If we do not have a subquery ongoing, we fetch a row and we start a new Subquery and ask it for hasMore.
 */
template <bool isModificationSubquery>
auto SubqueryExecutor<isModificationSubquery>::produceRows(AqlItemBlockInputRange& input,
                                                           OutputAqlItemRow& output)
    -> std::tuple<ExecutionState, Stats, AqlCall> {
  // We need to return skip in skipRows before
  TRI_ASSERT(_skipped == 0);

  auto getUpstreamCall = [&]() {
    AqlCall upstreamCall = output.getClientCall();
    if constexpr (isModificationSubquery) {
      upstreamCall = AqlCall{};
    }

    return upstreamCall;
  };

  INTERNAL_LOG_SQ << uint64_t(this) << "produceRows " << output.getClientCall();

  if (_state == ExecutorState::DONE && !_input.isInitialized()) {
    // We have seen DONE upstream, and we have discarded our local reference
    // to the last input, we will not be able to produce results anymore.
    return {translatedReturnType(), NoStats{}, getUpstreamCall()};
  }
  if (output.isFull()) {
    // This can happen if there is no upstream
    _state = input.upstreamState();
  }

  while (!output.isFull()) {
    if (_subqueryInitialized) {
      // Continue in subquery

      // Const case
      if (_infos.isConst() && !_input.isFirstDataRowInBlock()) {
        // Simply write
        writeOutput(output);
        INTERNAL_LOG_SQ << uint64_t(this) << "wrote output is const " << _state
                     << " " << getUpstreamCall();
        continue;
      }

      // Non const case, or first run in const
      auto [state, skipped, block] =
          _subquery.execute(AqlCallStack::emptyStack(), std::move(AqlCallList{AqlCall{}}));
      TRI_ASSERT(skipped.nothingSkipped());
      if (state == ExecutionState::WAITING) {
        return {state, NoStats{}, getUpstreamCall()};
      }
      // We get a result
      INTERNAL_LOG_SQ << uint64_t(this) << " we get subquery result";
      if (block != nullptr) {
        TRI_IF_FAILURE("SubqueryBlock::executeSubquery") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        if (_infos.returnsData()) {
          TRI_ASSERT(_subqueryResults != nullptr);
          INTERNAL_LOG_SQ << uint64_t(this)
                       << " store subquery result for writing " << block->size();
          _subqueryResults->emplace_back(std::move(block));
        }
      }

      // Subquery DONE
      if (state == ExecutionState::DONE) {
        writeOutput(output);
        INTERNAL_LOG_SQ << uint64_t(this) << "wrote output subquery done "
                     << _state << " " << getUpstreamCall();
      }
    } else {
      auto const [state, initialized] = initializeSubquery(input);
      if (state == ExecutionState::WAITING) {
        INTERNAL_LOG_SQ << "Waiting on initialize cursor";
        return {state, NoStats{}, AqlCall{}};
      }
      if (!initialized) {
        TRI_ASSERT(!_input);
        return {state, NoStats{}, getUpstreamCall()};
      }
      TRI_ASSERT(_subqueryInitialized);
    }
  }

  return {translatedReturnType(), NoStats{}, getUpstreamCall()};
}

template <bool isModificationSubquery>
void SubqueryExecutor<isModificationSubquery>::writeOutput(OutputAqlItemRow& output) {
  _subqueryInitialized = false;
  TRI_IF_FAILURE("SubqueryBlock::getSome") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_ASSERT(!output.isFull());
  if (!_infos.isConst() || _input.isFirstDataRowInBlock()) {
    // In the non const case we need to move the data into the output for every
    // row.
    // In the const case we need to move the data into the output once per block.
    TRI_ASSERT(_subqueryResults != nullptr);

    // We assert !returnsData => _subqueryResults is empty
    TRI_ASSERT(_infos.returnsData() || _subqueryResults->empty());
    AqlValue resultDocVec{_subqueryResults.get()};
    AqlValueGuard guard{resultDocVec, true};
    // Responsibility is handed over
    std::ignore = _subqueryResults.release();
    output.moveValueInto(_infos.outputRegister(), _input, guard);
    TRI_ASSERT(_subqueryResults == nullptr);
  } else {
    // In this case we can simply reference the last written value
    // We are not responsible for anything ourselves anymore
    TRI_ASSERT(_subqueryResults == nullptr);
    bool didReuse = output.reuseLastStoredValue(_infos.outputRegister(), _input);
    TRI_ASSERT(didReuse);
  }
  _input = InputAqlItemRow(CreateInvalidInputRowHint{});
  TRI_ASSERT(output.produced());
  output.advanceRow();
}

/// @brief shutdown, tell dependency and the subquery
template <bool isModificationSubquery>
std::pair<ExecutionState, Result> SubqueryExecutor<isModificationSubquery>::shutdown(int errorCode) {
  // Note this shutdown needs to be repeatable.
  // Also note the ordering of this shutdown is different
  // from earlier versions we now shutdown subquery first
  ExecutionState state = ExecutionState::DONE;
  if (!_shutdownDone) {
    // We take ownership of _state here for shutdown state
    std::tie(state, _shutdownResult) = _subquery.shutdown(errorCode);
    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(_shutdownResult.ok());
      return {ExecutionState::WAITING, TRI_ERROR_NO_ERROR};
    }
    _shutdownDone = true;
  }
  return {state, _shutdownResult};
}

template <bool isModificationSubquery>
auto SubqueryExecutor<isModificationSubquery>::translatedReturnType() const
    noexcept -> ExecutionState {
  if (_state == ExecutorState::DONE) {
    return ExecutionState::DONE;
  }
  return ExecutionState::HASMORE;
}

template <>
template <>
auto SubqueryExecutor<true>::skipRowsRange<>(AqlItemBlockInputRange& inputRange, AqlCall& call)
    -> std::tuple<ExecutionState, Stats, size_t, AqlCall> {
  INTERNAL_LOG_SQ << uint64_t(this) << "skipRowsRange " << call;

  if (_state == ExecutorState::DONE && !_input.isInitialized()) {
    // We have seen DONE upstream, and we have discarded our local reference
    // to the last input, we will not be able to produce results anymore.
    return {translatedReturnType(), NoStats{}, 0, AqlCall{}};
  }
  TRI_ASSERT(call.needSkipMore());
  // We cannot have a modifying subquery considered const
  TRI_ASSERT(!_infos.isConst());
  bool isFullCount = call.getLimit() == 0 && call.getOffset() == 0;
  while (isFullCount || _skipped < call.getOffset()) {
    if (_subqueryInitialized) {
      // Continue in subquery

      // While skipping we do not care for the result.
      // Simply jump over it.
      AqlCall subqueryCall{};
      subqueryCall.hardLimit = 0u;
      auto [state, skipRes, block] =
          _subquery.execute(AqlCallStack::emptyStack(), AqlCallList{subqueryCall});
      TRI_ASSERT(skipRes.nothingSkipped());
      if (state == ExecutionState::WAITING) {
        return {state, NoStats{}, 0, AqlCall{}};
      }
      // We get a result, but we asked for no rows.
      // so please give us no rows.
      TRI_ASSERT(block == nullptr);
      TRI_IF_FAILURE("SubqueryBlock::executeSubquery") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      // Subquery DONE
      if (state == ExecutionState::DONE) {
        _subqueryInitialized = false;
        _input = InputAqlItemRow(CreateInvalidInputRowHint{});
        _skipped += 1;
        INTERNAL_LOG_SQ << uint64_t(this) << "did skip one";
      }

    } else {
      auto const [state, initialized] = initializeSubquery(inputRange);
      if (state == ExecutionState::WAITING) {
        INTERNAL_LOG_SQ << "Waiting on initialize cursor";
        return {state, NoStats{}, 0, AqlCall{}};
      }
      if (!initialized) {
        TRI_ASSERT(!_input);
        if (state == ExecutionState::DONE) {
          // We are done, we will not get any more input.
          break;
        }
        return {state, NoStats{}, 0, AqlCall{}};
      }
      TRI_ASSERT(_subqueryInitialized);
    }
  }
  // If we get here, we are done with one set of skipping.
  // We either skipped the offset
  // or the fullCount
  // or both if limit == 0.
  call.didSkip(_skipped);
  _skipped = 0;
  return {translatedReturnType(), NoStats{}, call.getSkipCount(), AqlCall{}};
}

template <bool isModificationSubquery>
[[nodiscard]] auto SubqueryExecutor<isModificationSubquery>::expectedNumberOfRowsNew(
    AqlItemBlockInputRange const& input, AqlCall const& call) const noexcept -> size_t {
  if constexpr (isModificationSubquery) {
    // This executor might skip data.
    // It could overfetch it before.
    return std::min(call.getLimit(), input.countDataRows());
  }
  return input.countDataRows();
}

template class ::arangodb::aql::SubqueryExecutor<true>;
template class ::arangodb::aql::SubqueryExecutor<false>;
