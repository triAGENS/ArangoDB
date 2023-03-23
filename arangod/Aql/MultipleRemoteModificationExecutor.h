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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/InputAqlItemRow.h"
#include "Aql/ModificationExecutorHelpers.h"
#include "Aql/ModificationExecutorInfos.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/Stats.h"
#include "Transaction/Methods.h"

namespace arangodb {
namespace aql {
class ExecutionEngine;

struct MultipleRemoteModificationInfos : ModificationExecutorInfos {
  MultipleRemoteModificationInfos(
      ExecutionEngine* engine, RegisterId inputRegister,
      RegisterId outputNewRegisterId, RegisterId outputOldRegisterId,
      RegisterId outputRegisterId, arangodb::aql::QueryContext& query,
      OperationOptions options, aql::Collection const* aqlCollection,
      ConsultAqlWriteFilter consultAqlWriteFilter, IgnoreErrors ignoreErrors,
      IgnoreDocumentNotFound ignoreDocumentNotFound, bool hasParent)
      : ModificationExecutorInfos(
            engine, inputRegister, RegisterPlan::MaxRegisterId,
            RegisterPlan::MaxRegisterId, outputNewRegisterId,
            outputOldRegisterId, outputRegisterId, query, std::move(options),
            aqlCollection, ProducesResults(false), consultAqlWriteFilter,
            ignoreErrors, DoCount(true), IsReplace(false),
            ignoreDocumentNotFound),
        _hasParent(hasParent) {}

  bool _hasParent;  // node->hasParent();

  constexpr static double const defaultTimeOut = 3600.0;
};

struct MultipleRemoteModificationExecutor {
  struct Properties {
    static constexpr bool preservesOrder = true;
    static constexpr BlockPassthrough allowsBlockPassthrough =
        BlockPassthrough::Disable;
    static constexpr bool inputSizeRestrictsOutputSize = false;
  };
  using Infos = MultipleRemoteModificationInfos;
  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Stats =
      SingleRemoteModificationStats;  // TODO implement class for multiple

  MultipleRemoteModificationExecutor(Fetcher&, Infos&);
  ~MultipleRemoteModificationExecutor() = default;

  /**
   * @brief produce the next Row of Aql Values.
   *
   * @return ExecutionState,
   *         if something was written output.hasValue() == true
   */
  [[nodiscard]] auto produceRows(AqlItemBlockInputRange& input,
                                 OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, Stats, AqlCall>;
  [[nodiscard]] auto skipRowsRange(AqlItemBlockInputRange& input, AqlCall& call)
      -> std::tuple<ExecutorState, Stats, size_t, AqlCall>;

 protected:
  auto doMultipleRemoteModificationOperation(InputAqlItemRow&, Stats&)
      -> OperationResult;
  auto doMultipleRemoteModificationOutput(InputAqlItemRow&, OutputAqlItemRow&,
                                          OperationResult&) -> void;

  transaction::Methods _trx;
  Infos& _info;
  ExecutionState _upstreamState;
};

}  // namespace aql
}  // namespace arangodb
