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
/// @author Jan Steemann
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "Aql/JoinExecutor.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Collection.h"
#include "Aql/QueryContext.h"
#include "VocBase/LogicalCollection.h"

using namespace arangodb::aql;

JoinExecutor::~JoinExecutor() = default;

JoinExecutor::JoinExecutor(Fetcher& fetcher, Infos& infos)
    : _fetcher(fetcher), _infos(infos), _trx{_infos.query->newTrxContext()} {
  constructStrategy();
}

auto JoinExecutor::produceRows(AqlItemBlockInputRange& inputRange,
                               OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, Stats, AqlCall> {
  bool hasMore = false;
  while (inputRange.hasDataRow() && !output.isFull()) {
    if (!_currentRow) {
      std::tie(_currentRowState, _currentRow) = inputRange.peekDataRow();
      _strategy->reset();
    }

    hasMore = _strategy->next([&](std::span<LocalDocumentId> docIds,
                                  std::span<VPackSlice> projections) -> bool {
      // TODO post filtering based on projections
      // TODO store projections in registers

      for (std::size_t k = 0; k < docIds.size(); k++) {
        // TODO post filter based on document value
        // TODO implement document projections
        _infos.indexes[k].collection->getCollection()->getPhysical()->read(
            &_trx, docIds[k],
            [&](LocalDocumentId token, VPackSlice document) {
              AqlValue value{AqlValueHintSliceCopy{document}};
              AqlValueGuard guard(value, false);
              output.moveValueInto(_infos.indexes[k].documentOutputRegister,
                                   _currentRow, guard);
              return true;
            },
            ReadOwnWrites::no);
      }

      output.advanceRow();
      return !output.isFull();
    });

    if (!hasMore) {
      _currentRow = InputAqlItemRow{CreateInvalidInputRowHint{}};
    }

    inputRange.advanceDataRow();
  }

  return {inputRange.upstreamState(), Stats{}, AqlCall{}};
}

auto JoinExecutor::skipRowsRange(AqlItemBlockInputRange& inputRange,
                                 AqlCall& clientCall)
    -> std::tuple<ExecutorState, Stats, size_t, AqlCall> {
  bool hasMore = false;
  while (inputRange.hasDataRow() && clientCall.needSkipMore()) {
    if (!_currentRow) {
      std::tie(_currentRowState, _currentRow) = inputRange.peekDataRow();
      _strategy->reset();
    }

    hasMore = _strategy->next([&](std::span<LocalDocumentId> docIds,
                                  std::span<VPackSlice> projections) -> bool {
      // TODO post filtering based on projections
      for (std::size_t k = 0; k < docIds.size(); k++) {
        // TODO post filter based on document value
      }

      clientCall.didSkip(1);
      return clientCall.needSkipMore();
    });

    if (!hasMore) {
      _currentRow = InputAqlItemRow{CreateInvalidInputRowHint{}};
    }

    inputRange.advanceDataRow();
  }

  return {inputRange.upstreamState(), Stats{}, clientCall.getSkipCount(),
          AqlCall{}};
}

void JoinExecutor::constructStrategy() {
  std::vector<IndexJoinStrategyFactory::Descriptor> indexDescription;

  for (auto const& idx : _infos.indexes) {
    IndexStreamOptions options;
    // TODO right now we only support the first indexed field
    options.usedKeyFields = {0};
    auto stream = idx.index->streamForCondition(&_trx, options);
    TRI_ASSERT(stream != nullptr);

    auto& desc = indexDescription.emplace_back();
    desc.iter = std::move(stream);
    desc.numProjections = 0;  // idx.projectionOutputRegisters.size();
  }

  // TODO actually we want to have different strategies, like hash join and
  // special implementations for n = 2, 3, ...
  // TODO maybe make this an template parameter
  _strategy =
      IndexJoinStrategyFactory{}.createStrategy(std::move(indexDescription), 1);
}
