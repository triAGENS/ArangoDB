////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBEngine/RocksDBOptionsProvider.h"

#include "RocksDBEngine/RocksDBPrefixExtractor.h"

#include <rocksdb/slice_transform.h>

namespace arangodb {

RocksDBOptionsProvider::RocksDBOptionsProvider()
    : _vpackCmp(std::make_unique<RocksDBVPackComparator>()) {}

rocksdb::ColumnFamilyOptions RocksDBOptionsProvider::getColumnFamilyOptions(
    RocksDBColumnFamilyManager::Family family, rocksdb::Options const& base,
    rocksdb::BlockBasedTableOptions const& tableBase) const {
  rocksdb::ColumnFamilyOptions result(base);

  switch (family) {
    case RocksDBColumnFamilyManager::Family::Definitions:
    case RocksDBColumnFamilyManager::Family::Invalid:
      break;

    case RocksDBColumnFamilyManager::Family::Documents:
      // in the documents column family, it is totally unexpected to not
      // find a document by local document id. that means even in the lowest
      // levels we expect to find the document when looking it up.
      result.optimize_filters_for_hits = true;
      [[fallthrough]];

    case RocksDBColumnFamilyManager::Family::PrimaryIndex:
    case RocksDBColumnFamilyManager::Family::GeoIndex:
    case RocksDBColumnFamilyManager::Family::FulltextIndex:
    case RocksDBColumnFamilyManager::Family::ZkdIndex:
    case RocksDBColumnFamilyManager::Family::ReplicatedLogs: {
      // fixed 8 byte object id prefix
      result.prefix_extractor = std::shared_ptr<rocksdb::SliceTransform const>(
          rocksdb::NewFixedPrefixTransform(RocksDBKey::objectIdSize()));
      break;
    }

    case RocksDBColumnFamilyManager::Family::EdgeIndex: {
      result.prefix_extractor = std::make_shared<RocksDBPrefixExtractor>();
      // also use hash-search based SST file format
      rocksdb::BlockBasedTableOptions tableOptions(tableBase);
      tableOptions.index_type =
          rocksdb::BlockBasedTableOptions::IndexType::kHashSearch;
      result.table_factory = std::shared_ptr<rocksdb::TableFactory>(
          rocksdb::NewBlockBasedTableFactory(tableOptions));
      break;
    }
    case RocksDBColumnFamilyManager::Family::VPackIndex: {
      // velocypack based index variants with custom comparator
      rocksdb::BlockBasedTableOptions tableOptions(tableBase);
      tableOptions.filter_policy.reset();  // intentionally no bloom filter here
      result.table_factory = std::shared_ptr<rocksdb::TableFactory>(
          rocksdb::NewBlockBasedTableFactory(tableOptions));
      result.comparator = _vpackCmp.get();
      break;
    }
  }

  return result;
}
}  // namespace arangodb
