////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_AQL_READ_ALL_EXECUTIONBLOCK_H
#define ARANGOD_AQL_READ_ALL_EXECUTIONBLOCK_H 1

#include "Aql/ExecutionBlock.h"

#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/ScatterExecutor.h"

#include <deque>

namespace arangodb {
namespace aql {

class QueryContext;

class ReadAllExecutionBlock : public ExecutionBlock {
public:
ReadAllExecutionBlock(ExecutionEngine*, ExecutionNode const*, RegisterInfos registerInfos);

~ReadAllExecutionBlock();

  /// @brief main function to produce data in this ExecutionBlock.
  ///        It gets the AqlCallStack defining the operations required in every
  ///        subquery level. It will then perform the requested amount of offset, data and fullcount.
  ///        The AqlCallStack is copied on purpose, so this block can modify it.
  ///        Will return
  ///        1. state:
  ///          * WAITING: We have async operation going on, nothing happend, please call again
  ///          * HASMORE: Here is some data in the request range, there is still more, if required call again
  ///          * DONE: Here is some data, and there will be no further data available.
  ///        2. SkipResult: Amount of documents skipped.
  ///        3. SharedAqlItemBlockPtr: The next data block.
  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(AqlCallStack stack) override;
  

  private:
  auto executeWithoutTrace(AqlCallStack stack) -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  [[nodiscard]] QueryContext const& getQuery() const;

private:
  QueryContext const& _query;

  SkipResult _skipped{};

  ScatterExecutor::ClientBlockData _clientBlockData;
  
  std::deque<SharedAqlItemBlockPtr> _blocks;
};

}  // namespace aql
}  // namespace arangodb
#endif