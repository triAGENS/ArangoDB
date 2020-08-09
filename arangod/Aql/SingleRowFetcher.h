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

#ifndef ARANGOD_AQL_SINGLE_ROW_FETCHER_H
#define ARANGOD_AQL_SINGLE_ROW_FETCHER_H

#include "Aql/AqlItemBlockInputRange.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Aql/types.h"

#include <memory>

namespace arangodb::aql {

class AqlItemBlock;
template <BlockPassthrough>
class DependencyProxy;
class SkipResult;

/**
 * @brief Interface for all AqlExecutors that do only need one
 *        row at a time in order to make progress.
 */
template <BlockPassthrough blockPassthrough>
class SingleRowFetcher {
 public:
  explicit SingleRowFetcher(DependencyProxy<blockPassthrough>& executionBlock);
  TEST_VIRTUAL ~SingleRowFetcher() = default;

  using DataRange = AqlItemBlockInputRange;

 protected:
  // only for testing! Does not initialize _dependencyProxy!
  SingleRowFetcher();

 public:
  /**
   * @brief Execute the given call stack
   *
   * @param stack Call stack, on top of stack there is current subquery, bottom is the main query.
   * @return std::tuple<ExecutionState, size_t, DataRange>
   *   ExecutionState => DONE, all queries are done, there will be no more
   *   ExecutionState => HASMORE, there are more results for queries, might be on other subqueries
   *   ExecutionState => WAITING, we need to do I/O to solve the request, save local state and return WAITING to caller immediately
   *
   *   size_t => Amount of documents skipped
   *   DataRange => Resulting data
   */
  std::tuple<ExecutionState, SkipResult, DataRange> execute(AqlCallStack const& stack, AqlCallList clientCall);

  void setDistributeId(std::string const& id);

 private:
  DependencyProxy<blockPassthrough>* _dependencyProxy;

};
}  // namespace arangodb::aql

#endif  // ARANGOD_AQL_SINGLE_ROW_FETCHER_H
