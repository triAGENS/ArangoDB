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

#ifndef ARANGOD_AQL_BLOCK_FETCHER_H
#define ARANGOD_AQL_BLOCK_FETCHER_H

#include "Aql/AqlCallStack.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/types.h"

#include <memory>
#include <queue>
#include <unordered_set>
#include <utility>

namespace arangodb::aql {
class ExecutionBlock;
class AqlItemBlockManager;
class SkipResult;

/**
 * @brief Thin interface to access the methods of ExecutionBlock that are
 * necessary for the row Fetchers. Makes it easier to test the Fetchers.
 */
template <BlockPassthrough allowBlockPassthrough>
class DependencyProxy {
 public:
  /**
   * @brief Interface to fetch AqlItemBlocks from upstream with getSome.
   * @param dependencies Dependencies of the current ExecutionBlock. Must
   *                     contain EXACTLY ONE element. Otherwise, DependencyProxy
   *                     may be instantiated, but never used. It is allowed to
   *                     pass a reference to an empty vector, but as soon as
   *                     the DependencyProxy is used, the condition must be
   *                     satisfied.
   * @param nrInputRegisters Total number of registers of the AqlItemBlocks
   *                         here. Called nrInputRegisters to discern between
   *                         the widths of input and output blocks.
   *
   * The constructor MAY NOT access the dependencies, nor the itemBlockManager.
   * This is because the dependencies will be added to the ExecutionBlock only
   * after construction, and to allow derived subclasses for testing (read
   * DependencyProxyMock) to create them *after* the parent class was constructed.
   */
  DependencyProxy(std::vector<ExecutionBlock*> const& dependencies,
                  RegisterCount nrInputRegisters);

  TEST_VIRTUAL ~DependencyProxy() = default;

  TEST_VIRTUAL std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(
      AqlCallStack const& stack, AqlCallList clientCall);

  TEST_VIRTUAL std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> executeForDependency(
      size_t dependency, AqlCallStack const& stack, AqlCallList clientCall);

  [[nodiscard]] RegisterCount getNrInputRegisters() const;

  [[nodiscard]] TEST_VIRTUAL size_t numberDependencies() const;

  void reset();

  void setDistributeId(std::string const& distId) { _distributeId = distId; }

 protected:
  [[nodiscard]] ExecutionBlock& upstreamBlock();

  [[nodiscard]] ExecutionBlock& upstreamBlockForDependency(size_t index);

 private:
  [[nodiscard]] bool advanceDependency();

 private:
  std::vector<ExecutionBlock*> const& _dependencies;
  RegisterCount const _nrInputRegisters;
  std::string _distributeId;

  // only modified in case of multiple dependencies + Passthrough otherwise always 0
  size_t _currentDependency;
};

}  // namespace arangodb::aql

#endif  // ARANGOD_AQL_BLOCK_FETCHER_H
