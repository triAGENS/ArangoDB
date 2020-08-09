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

#ifndef ARANGOD_AQL_TESTS_DEPENDENCY_PROXY_MOCK_H
#define ARANGOD_AQL_TESTS_DEPENDENCY_PROXY_MOCK_H

#include "Aql/AqlItemBlockManager.h"
#include "Aql/DependencyProxy.h"
#include "Aql/ExecutionState.h"
#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/types.h"

#include <cstdint>
#include <queue>

namespace arangodb {
namespace aql {
class SkipResult;
}
namespace tests {
namespace aql {

template <::arangodb::aql::BlockPassthrough passBlocksThrough>
class DependencyProxyMock : public ::arangodb::aql::DependencyProxy<passBlocksThrough> {
 public:
  explicit DependencyProxyMock(arangodb::aql::ResourceMonitor& monitor,
                               ::arangodb::aql::RegisterId nrRegisters);

 public:
  // mock methods
  inline size_t numberDependencies() const override { return 1; }

  std::tuple<arangodb::aql::ExecutionState, arangodb::aql::SkipResult, arangodb::aql::SharedAqlItemBlockPtr> execute(
      arangodb::aql::AqlCallStack const& stack, arangodb::aql::AqlCallList clientCall) override;

 private:
  using FetchBlockReturnItem =
      std::pair<arangodb::aql::ExecutionState, arangodb::aql::SharedAqlItemBlockPtr>;

 public:
  // additional test methods
  DependencyProxyMock& shouldReturn(arangodb::aql::ExecutionState,
                                    arangodb::aql::SharedAqlItemBlockPtr const&);
  DependencyProxyMock& shouldReturn(FetchBlockReturnItem);
  DependencyProxyMock& shouldReturn(std::vector<FetchBlockReturnItem>);
  DependencyProxyMock& andThenReturn(FetchBlockReturnItem);
  DependencyProxyMock& andThenReturn(arangodb::aql::ExecutionState,
                                     arangodb::aql::SharedAqlItemBlockPtr const&);
  DependencyProxyMock& andThenReturn(std::vector<FetchBlockReturnItem>);

  bool allBlocksFetched() const;
  size_t numFetchBlockCalls() const;

 private:
  std::queue<FetchBlockReturnItem> _itemsToReturn;

  size_t _numFetchBlockCalls;

  ::arangodb::aql::ResourceMonitor& _monitor;
  ::arangodb::aql::AqlItemBlockManager _itemBlockManager;
  ::arangodb::aql::SharedAqlItemBlockPtr _block;
};

template <::arangodb::aql::BlockPassthrough passBlocksThrough>
class MultiDependencyProxyMock
    : public ::arangodb::aql::DependencyProxy<passBlocksThrough> {
 public:
  MultiDependencyProxyMock(arangodb::aql::ResourceMonitor& monitor,
                           ::arangodb::aql::RegIdSet const& inputRegisters,
                           ::arangodb::aql::RegisterId nrRegisters, size_t nrDeps);

 public:
  // mock methods

  inline size_t numberDependencies() const override {
    return _dependencyMocks.size();
  }

 public:
  // additional test methods
  DependencyProxyMock<passBlocksThrough>& getDependencyMock(size_t dependency) {
    TRI_ASSERT(dependency < _dependencyMocks.size());
    return *_dependencyMocks[dependency];
  }
  bool allBlocksFetched() const;

  size_t numFetchBlockCalls() const;

 private:
  std::vector<std::unique_ptr<DependencyProxyMock<passBlocksThrough>>> _dependencyMocks;
  ::arangodb::aql::AqlItemBlockManager _itemBlockManager;
};

}  // namespace aql
}  // namespace tests
}  // namespace arangodb

#endif  // ARANGOD_AQL_TESTS_DEPENDENCY_PROXY_MOCK_H
