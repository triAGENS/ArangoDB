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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_TESTS_ROW_FETCHER_HELPER_H
#define ARANGOD_AQL_TESTS_ROW_FETCHER_HELPER_H

#include "Aql/AllRowsFetcher.h"
#include "Aql/AqlItemBlockManager.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/ResourceUsage.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/VelocyPackHelper.h"

#include <Basics/Common.h>
#include <velocypack/Buffer.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

namespace arangodb {

namespace aql {
class AqlItemBlock;
class AqlItemBlockManager;
class InputAqlItemRow;
class AqlItemMatrix;
}  // namespace aql

namespace tests {
namespace aql {

/**
 * @brief Mock for SingleRowFetcher
 */
template <::arangodb::aql::BlockPassthrough passBlocksThrough>
class SingleRowFetcherHelper
    : public arangodb::aql::SingleRowFetcher<passBlocksThrough> {
 public:
  SingleRowFetcherHelper(arangodb::aql::AqlItemBlockManager& manager,
                         size_t blockSize, bool returnsWaiting,
                         arangodb::aql::SharedAqlItemBlockPtr input);

  // backwards compatible constructor
  SingleRowFetcherHelper(arangodb::aql::AqlItemBlockManager& manager,
                         std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> const& vPackBuffer,
                         bool returnsWaiting);

  virtual ~SingleRowFetcherHelper();

  uint64_t nrCalled() { return _nrCalled; }
  uint64_t nrReturned() { return _nrReturned; }
  uint64_t nrItems() { return _nrItems; }

  size_t totalSkipped() const { return _totalSkipped; }

  arangodb::aql::AqlItemBlockManager& itemBlockManager() {
    return _itemBlockManager;
  }

  bool isDone() const { return _returnedDoneOnFetchRow; };

 private:
  arangodb::aql::SharedAqlItemBlockPtr& getItemBlock() { return _itemBlock; }
  arangodb::aql::SharedAqlItemBlockPtr const& getItemBlock() const {
    return _itemBlock;
  }

  void nextRow() {
    _curRowIndex++;
    _curIndexInBlock = (_curIndexInBlock + 1) % _blockSize;
  }

  bool wait() {
    // Wait on the first row of every block, if applicable
    if (_returnsWaiting && _curIndexInBlock == 0) {
      // If the insert succeeds, we have not yet waited at this index.
      bool const waitNow = _didWaitAt.insert(_curRowIndex).second;

      return waitNow;
    }

    return false;
  }

 private:
  bool _returnedDoneOnFetchRow = false;
  bool _returnedDoneOnFetchShadowRow = false;
  bool const _returnsWaiting;
  uint64_t _nrItems;
  uint64_t _nrCalled{};
  uint64_t _nrReturned{};
  size_t _skipped{};
  size_t _totalSkipped{};
  size_t _curIndexInBlock{};
  size_t _curRowIndex{};
  size_t _blockSize;
  std::unordered_set<size_t> _didWaitAt;
  arangodb::aql::AqlItemBlockManager& _itemBlockManager;
  arangodb::aql::SharedAqlItemBlockPtr _itemBlock;
  arangodb::aql::InputAqlItemRow _lastReturnedRow{arangodb::aql::CreateInvalidInputRowHint{}};
};

/**
 * @brief Mock for AllRowsFetcher
 */
class AllRowsFetcherHelper : public arangodb::aql::AllRowsFetcher {
 public:
  AllRowsFetcherHelper(std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> vPackBuffer,
                       bool returnsWaiting);
  ~AllRowsFetcherHelper();

 private:
  std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> _vPackBuffer;
  arangodb::velocypack::Slice _data;
  uint64_t _nrItems;
  arangodb::aql::RegisterCount _nrRegs;
  arangodb::aql::ResourceMonitor _resourceMonitor;
  arangodb::aql::AqlItemBlockManager _itemBlockManager;
  std::unique_ptr<arangodb::aql::AqlItemMatrix> _matrix;
};

class ConstFetcherHelper : public arangodb::aql::ConstFetcher {
 public:
  ConstFetcherHelper(arangodb::aql::AqlItemBlockManager& itemBlockManager,
                     std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> vPackBuffer);
  virtual ~ConstFetcherHelper();

 private:
  std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> _vPackBuffer;
  arangodb::velocypack::Slice _data;
};

}  // namespace aql
}  // namespace tests
}  // namespace arangodb

#endif
