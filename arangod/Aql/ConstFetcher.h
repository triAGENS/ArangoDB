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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_CONST_FETCHER_H
#define ARANGOD_AQL_CONST_FETCHER_H

#include "Aql/AqlItemBlockInputRange.h"
#include "Aql/ExecutionState.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/SkipResult.h"
#include "Containers/SmallVector.h"

#include <memory>

namespace arangodb {
namespace aql {

class AqlCallList;
class AqlCallStack;
class AqlItemBlock;
template <BlockPassthrough>
class DependencyProxy;
class ShadowAqlItemRow;

/**
 * @brief Interface for all AqlExecutors that do only need one
 *        row at a time in order to make progress.
 */
class ConstFetcher {
  using DependencyProxy = aql::DependencyProxy<BlockPassthrough::Enable>;

 public:
  using DataRange = AqlItemBlockInputRange;
  explicit ConstFetcher(DependencyProxy& executionBlock);
  TEST_VIRTUAL ~ConstFetcher() = default;

 protected:
  // only for testing! Does not initialize _dependencyProxy!
  ConstFetcher();

 public:
  /**
   * @brief Execute the given call stack
   *
   * @param stack Outer subqueries call stack, bottom is the main query.
   * @param clientCall The call relevant for this executor
   * @return std::tuple<ExecutionState, size_t, DataRange>
   *   ExecutionState => DONE, all queries are done, there will be no more
   *   ExecutionState => HASMORE, there are more results for queries, might be on other subqueries
   *   ExecutionState => WAITING, we need to do I/O to solve the request, save local state and return WAITING to caller immediately
   *
   *   size_t => Amount of documents skipped
   *   DataRange => Resulting data
   */
  auto execute(AqlCallStack const& stack, AqlCallList clientCall) -> std::tuple<ExecutionState, SkipResult, DataRange>;

  void injectBlock(SharedAqlItemBlockPtr block, SkipResult skipped);

  void setDistributeId(std::string const&) {
    // This is not implemented for this fetcher
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

 private:
  /**
   * @brief Input block currently in use. Used for memory management by the
   *        ConstFetcher. May be moved if the Fetcher implementations
   *        are moved into separate classes.
   */
  SharedAqlItemBlockPtr _currentBlock;

  /**
   * @brief The amount of documents skipped in outer subqueries.
   *
   */
  SkipResult _skipped;

  SharedAqlItemBlockPtr _blockForPassThrough;

  /**
   * @brief Index of the row to be returned next. This is valid
   *        iff _currentBlock != nullptr and it's smaller or equal than
   *        _currentBlock->size(). May be moved if the Fetcher implementations
   *        are moved into separate classes.
   */
  size_t _rowIndex;

 private:
  auto indexIsValid() const noexcept -> bool;
  auto numRowsLeft() const noexcept -> size_t;
  auto canUseFullBlock(arangodb::containers::SmallVector<std::pair<size_t, size_t>> const& ranges) const
      noexcept -> bool;
};

}  // namespace aql
}  // namespace arangodb

#endif  // ARANGOD_AQL_SINGLE_ROW_FETCHER_H
