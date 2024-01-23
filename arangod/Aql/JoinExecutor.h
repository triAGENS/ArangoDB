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

#pragma once

#include "Aql/AqlCall.h"
#include "Aql/AqlItemBlockInputRange.h"
#include "Aql/Ast.h"
#include "Aql/DocumentProducingHelper.h"
#include "Aql/ExecutionState.h"
#include "Aql/IndexJoinStrategy.h"
#include "Aql/JoinNode.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/NonConstExpressionContainer.h"
#include "Aql/RegisterId.h"
#include "Aql/RegisterInfos.h"
#include "Aql/Stats.h"
#include "Aql/Variable.h"
#include "Containers/FlatHashMap.h"
#include "Transaction/Methods.h"

#include <velocypack/Builder.h>

#include <memory>
#include <optional>
#include <vector>

namespace arangodb {
class IndexIterator;
struct ResourceMonitor;

namespace aql {

class ExecutionEngine;
class ExecutorExpressionContext;
class RegisterInfos;
class Expression;
class InputAqlItemRow;
class Projections;
class QueryContext;

template<BlockPassthrough>
class SingleRowFetcher;

struct AstNode;
struct Collection;
struct NonConstExpression;

struct JoinExecutorInfos {
  struct IndexInfo {
    // Register to load the document into
    RegisterId documentOutputRegister;
    RegisterId docIdOutputRegister;
    bool isLateMaterialized;

    // Associated document collection for this index
    Collection const* collection;
    // Index handle
    transaction::Methods::IndexHandle index;

    // Values used for projection
    Projections projections;

    bool hasProjectionsForRegisters = false;
    bool producesOutput = true;

    struct FilterInformation {
      // post filter expression
      std::unique_ptr<Expression> expression;

      // variable in the expression referring to the current document
      Variable const* documentVariable;

      // mapping of other variables to register in the input row
      std::vector<std::pair<VariableId, RegisterId>> filterVarsToRegs;

      // projections used for filtering
      Projections projections;
      std::vector<Variable const*> filterProjectionVars;
    };

    std::optional<FilterInformation> filter;

    // used for jumping to the correct location during reset calls
    std::vector<std::unique_ptr<Expression>> constantExpressions;
    // mapping of other variables to register in the input row
    std::vector<std::pair<VariableId, RegisterId>> expressionVarsToRegs;

    std::vector<size_t> usedKeyFields;
    std::vector<size_t> constantFields;
  };

  RegisterId registerForVariable(VariableId id) const noexcept;
  void determineProjectionsForRegisters();

  std::vector<IndexInfo> indexes;
  QueryContext* query;
  containers::FlatHashMap<VariableId, RegisterId> varsToRegister;
  bool projectionsInitialized = false;
  bool producesAnyOutput = true;
};

/**
 * @brief Implementation of Join Node
 */
class JoinExecutor {
 public:
  struct Properties {
    static constexpr bool preservesOrder = false;
    static constexpr BlockPassthrough allowsBlockPassthrough =
        BlockPassthrough::Disable;
  };

  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Infos = JoinExecutorInfos;
  using Stats = JoinStats;

  JoinExecutor() = delete;
  JoinExecutor(JoinExecutor&&) = delete;
  JoinExecutor(JoinExecutor const&) = delete;
  JoinExecutor(Fetcher& fetcher, Infos&);
  ~JoinExecutor();

  auto produceRows(AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, Stats, AqlCall>;

  auto skipRowsRange(AqlItemBlockInputRange& inputRange, AqlCall& clientCall)
      -> std::tuple<ExecutorState, Stats, size_t, AqlCall>;

 private:
  void constructStrategy();
  [[nodiscard]] ResourceMonitor& resourceMonitor();

  aql::AqlFunctionsInternalCache _functionsCache;
  Infos& _infos;
  std::unique_ptr<AqlIndexJoinStrategy> _strategy;

  transaction::Methods _trx;
  ResourceMonitor& _resourceMonitor;

  InputAqlItemRow _currentRow{CreateInvalidInputRowHint()};
  ExecutorState _currentRowState{ExecutorState::HASMORE};
  velocypack::Builder _projectionsBuilder;
  // first value holds the unique ptr to a string (obvious), second value holds
  // the amount of bytes used by that string
  std::vector<std::pair<std::unique_ptr<std::string>, size_t>> _documents;

  // used for constant expressions, will stay a
  VPackBuilder _constantBuilder;
  std::vector<VPackSlice> _constantSlices;
};

}  // namespace aql
}  // namespace arangodb
