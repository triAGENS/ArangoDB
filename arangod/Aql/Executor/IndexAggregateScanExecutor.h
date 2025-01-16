////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2025 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Aql/AqlFunctionsInternalCache.h"
#include "Aql/IndexStreamIterator.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/Stats.h"
#include "Aql/QueryContext.h"
#include "Aql/Expression.h"
#include "Transaction/Methods.h"

namespace arangodb {

namespace aql {
struct AqlCall;
struct Aggregator;
class OutputAqlItemRow;

struct IndexAggregateScanInfos {
  // Associated document collection for this index
  Collection const* collection;
  // Index handle
  transaction::Methods::IndexHandle index;

  struct Group {
    RegisterId outputRegister;
    size_t indexField;
  };
  std::vector<Group> groups;

  struct Aggregation {
    std::string type;
    RegisterId outputRegister;
    std::unique_ptr<Expression> expression;
  };

  std::vector<Aggregation> aggregations;

  containers::FlatHashMap<VariableId, size_t> _expressionVariables;

  QueryContext* query;
};

struct IndexAggregateScanStats {
  void operator+=(IndexAggregateScanStats const& stats) noexcept {}
};

inline ExecutionStats& operator+=(
    ExecutionStats& executionStats,
    IndexAggregateScanStats const& stats) noexcept {
  return executionStats;
}

struct IndexAggregateScanExecutor {
  struct Properties {
    static constexpr bool preservesOrder = false;
    static constexpr BlockPassthrough allowsBlockPassthrough =
        BlockPassthrough::Disable;
  };
  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Stats = IndexAggregateScanStats;
  using Infos = IndexAggregateScanInfos;

  IndexAggregateScanExecutor() = delete;
  IndexAggregateScanExecutor(IndexAggregateScanExecutor&&) = delete;
  IndexAggregateScanExecutor(IndexAggregateScanExecutor const&) = delete;
  IndexAggregateScanExecutor(Fetcher& fetcher, Infos&);

  auto produceRows(AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, Stats, AqlCall>;

  auto skipRowsRange(AqlItemBlockInputRange& inputRange, AqlCall& clientCall)
      -> std::tuple<ExecutorState, Stats, size_t, AqlCall>;

 private:
  Fetcher& _fetcher;
  Infos& _infos;
  transaction::Methods _trx;
  // iterator on index that includes all infos that are needed for the groups
  std::unique_ptr<AqlIndexStreamIterator> _iterator;

  std::vector<std::unique_ptr<Aggregator>> _aggregatorInstances;

  std::vector<VPackSlice> _currentGroupKeySlices;
  std::vector<VPackSlice> _keySlices;
  std::vector<VPackSlice> _projectionSlices;
  InputAqlItemRow _inputRow{CreateInvalidInputRowHint{}};
  aql::AqlFunctionsInternalCache _functionsCache;
  containers::FlatHashMap<VariableId, size_t> _variablesToProjectionsRelative;
};

}  // namespace aql
}  // namespace arangodb
