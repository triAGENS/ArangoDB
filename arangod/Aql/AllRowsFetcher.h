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

#ifndef ARANGOD_AQL_ALL_ROWS_FETCHER_H
#define ARANGOD_AQL_ALL_ROWS_FETCHER_H

#include "Aql/AqlItemMatrix.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/types.h"

#include <Basics/Common.h>
#include <Basics/Exceptions.h>

#include <cstddef>
#include <memory>

#include "Aql/AqlItemBlockInputMatrix.h"

namespace arangodb {
namespace aql {

class AqlItemBlock;
class SharedAqlItemBlockPtr;
enum class ExecutionState;
template <BlockPassthrough>
class DependencyProxy;
class ShadowAqlItemRow;
class SkipResult;

/**
 * @brief Interface for all AqlExecutors that do need all
 *        rows at a time in order to make progress.
 *
 *
 * Class Description and Guarantees
 * - Will have a single DependencyProxy only. This DependencyProxy cannot be PassThrough.
 * - It will offer the following APIs:
 *   - fetchAllRows()
 *     => Will fetch all Rows from the DependencyProxy until a ShadowRow is fetched
 *     => If any of the requests to DependencyProxy returns WAITING, this will be forwarded.
 *     => If all rows have been Fetched, it will return DONE and an AqlItemMatrix, the Matrix will return results
 *     => Any later call will return DONE and a nullptr. So make sure you keep the Matrix.
 *     => This state can be left only if the shadowRow is fetched.
 *   - preFetchNumberOfRows()
 *     => Will do the same as fetchAllRows, but NOT give out the data, it will only hold it internally.
 *     => On response it will inform the caller on exactly how many Rows will be returned until the next ShadowRow appears.
 *   - upstreamState()
 *     => Returns the last state of the dependencyProxy.
 *   - fetchShadowRow()
 *     => Can only be called after fetchAllRows()
 *     => It is supposed to pop the next shadowRow, namely the reason why fetchAllRows() was done.
 *     => You should continue to call fetchShadowRow() until you get either DONE or an invalid ShadowRow, as it could be possible that a higher level ShadowRow is next
 *     => If this call returns DONE your query is DONE and you can be sure no further Rows will be produced.
 *     => After this is called fetchAllRows() can return a new AqlMatrix again.
 *     => NOTE: If you have releveant ShadowRows in consecutive order, you are required to call fetchAllRows() in between of them. (This is required for COLLECT which needs to produce a row on 0 input).
 *
 *
 * - This class should be used if the Executor requires that ALL input is produced before it can start to work, e.g. it gives guarantees on side effects or needs to do Sorting.
 */
class AllRowsFetcher {
 private:
 public:
  explicit AllRowsFetcher(DependencyProxy<BlockPassthrough::Disable>& executionBlock);
  TEST_VIRTUAL ~AllRowsFetcher() = default;

  using DataRange = AqlItemBlockInputMatrix;

 protected:
  // only for testing! Does not initialize _dependencyProxy!
  AllRowsFetcher() = default;

 public:
  /**
   * @brief Execute the given call stack
   *
   * @param stack Call stack, all calls of outer subqueries
   * @param clientCall current subquery information
   * @return std::tuple<ExecutionState, size_t, DataRange>
   *   ExecutionState => DONE, all queries are done, there will be no more
   *   ExecutionState => HASMORE, there are more results for queries, might be on other subqueries
   *   ExecutionState => WAITING, we need to do I/O to solve the request, save local state and return WAITING to caller immediately
   *
   *   size_t => Amount of documents skipped
   *   DataRange => Resulting data
   */
  std::tuple<ExecutionState, SkipResult, DataRange> execute(AqlCallStack const& stack, AqlCallList clientCall);

  // only for ModificationNodes
  ExecutionState upstreamState();

 private:
  DependencyProxy<BlockPassthrough::Disable>* _dependencyProxy;

  std::unique_ptr<AqlItemMatrix> _aqlItemMatrix;
  ExecutionState _upstreamState;
  std::size_t _blockToReturnNext;

 private:
  /**
   * @brief Delegates to ExecutionBlock::getNrInputRegisters()
   */
  RegisterCount getNrInputRegisters() const;
};

}  // namespace aql
}  // namespace arangodb

#endif  // ARANGOD_AQL_ALL_ROWS_FETCHER_H
