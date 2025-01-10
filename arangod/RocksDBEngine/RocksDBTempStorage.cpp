////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Julia Puget
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBTempStorage.h"

#include "Basics/FileUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Basics/files.h"
#include "Logger/LogMacros.h"
#include "RestServer/TemporaryStorageFeature.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBSortedRowsStorageContext.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/EncryptionProvider.h"
#include "Enterprise/RocksDBEngine/RocksDBEncryptionUtilsEE.h"
#endif

#include <absl/strings/str_cat.h>

#include <rocksdb/cache.h>
#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/env_encryption.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>

using namespace arangodb;

namespace {

class KeysComparator : public rocksdb::Comparator {
 public:
  KeysComparator() = default;
  int Compare(rocksdb::Slice const& lhs,
              rocksdb::Slice const& rhs) const override {
    TRI_ASSERT(lhs.size() >= sizeof(uint64_t));
    TRI_ASSERT(rhs.size() >= sizeof(uint64_t));
    // compare first 8 bytes, which is the "context id"
    int diff = memcmp(lhs.data(), rhs.data(), sizeof(uint64_t));
    if (diff != 0) {
      return diff;
    }

    // move beyond "context id"
    auto p1 = lhs.data() + sizeof(uint64_t);
    auto p2 = rhs.data() + sizeof(uint64_t);

    // now compare the running number (used for stable sorts)
    int diffInId = 0;
    bool hasPrefixId1 =
        static_cast<size_t>(p1 - lhs.data()) + sizeof(uint64_t) <= lhs.size();
    bool hasPrefixId2 =
        static_cast<size_t>(p2 - rhs.data()) + sizeof(uint64_t) <= rhs.size();

    if (hasPrefixId1 && hasPrefixId2) {
      diffInId = memcmp(p1, p2, sizeof(uint64_t));
      p1 += sizeof(uint64_t);
      p2 += sizeof(uint64_t);
    } else if (hasPrefixId1) {
      diffInId = 1;
    } else if (hasPrefixId2) {
      diffInId = -1;
    }
    // here, we expect always pairs of the actual key value to compare (the
    // slice) + the byte that represents the order in which to compare. As we
    // build the arguments ourselves to call this comparator, we can expect that
    // either there would be no more value with order to compare, or the value
    // would be present with the order byte
    while (static_cast<size_t>(p1 - lhs.data()) < lhs.size() &&
           static_cast<size_t>(p2 - rhs.data()) < rhs.size()) {
      velocypack::Slice slice1(reinterpret_cast<uint8_t const*>(p1));
      p1 += slice1.byteSize();
      velocypack::Slice slice2(reinterpret_cast<uint8_t const*>(p2));
      p2 += slice2.byteSize();
      char order1 = *p1;
      char order2 = *p2;
      TRI_ASSERT(order1 == order2);

      diff = basics::VelocyPackHelper::compare(slice1, slice2, true);

      if (diff != 0) {
        return (order1 == '1') ? diff : -diff;
      }
      ++p1;
      ++p2;
    }

    // if everything else is equal, return the difference of the
    // bytes 8-15, which is the insertion id. this makes the result
    // predictable when there are multiple identical keys (stable sort).
    return diffInId;
  }

  // Ignore the following methods for now:
  char const* Name() const override { return "KeysComparator"; }
  void FindShortestSeparator(std::string*,
                             rocksdb::Slice const&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

}  // namespace

RocksDBTempStorage::RocksDBTempStorage(std::string const& basePath,
                                       StorageUsageTracker& usageTracker,
                                       bool useEncryption,
                                       bool allowHWAcceleration)
    : _basePath(basePath),
      _usageTracker(usageTracker),
#ifdef USE_ENTERPRISE
      _useEncryption(useEncryption),
      _allowHWAcceleration(allowHWAcceleration),
#endif
      _nextId(0),
      _db(nullptr),
      _comparator(std::make_unique<::KeysComparator>()) {
}

RocksDBTempStorage::~RocksDBTempStorage() { close(); }

Result RocksDBTempStorage::init() {
  // path for temporary files, not managed by RocksDB, but by us.
  _tempFilesPath = basics::FileUtils::buildFilename(_basePath, "temp");

  {
    std::string systemErrorStr;
    long errorNo;

    auto res = TRI_CreateRecursiveDirectory(_tempFilesPath.c_str(), errorNo,
                                            systemErrorStr);

    if (res != TRI_ERROR_NO_ERROR) {
      return {
          TRI_ERROR_FAILED,
          absl::StrCat("cannot create directory for intermediate results ('",
                       _tempFilesPath, "'): ", systemErrorStr)};
    }
  }

  rocksdb::Options options;
  options.create_missing_column_families = true;
  options.create_if_missing = true;
  options.env = rocksdb::Env::Default();

#ifdef USE_ENTERPRISE
  if (_useEncryption) {
    // set up Env for encryption
    try {
      // Setup block cipher
      std::string encryptionKey =
          enterprise::EncryptionUtils::generateRandomKey();

      _encryptionProvider = std::make_shared<enterprise::EncryptionProvider>(
          encryptionKey, _allowHWAcceleration);
    } catch (std::exception const& ex) {
      LOG_TOPIC("91e3c", FATAL, arangodb::Logger::STARTUP)
          << "error while creating encryption cipher: " << ex.what();
      FATAL_ERROR_EXIT();
    }

    _encryptedEnv.reset(NewEncryptedEnv(options.env, _encryptionProvider));
    options.env = _encryptedEnv.get();
  }
#endif

  // set per-level compression. we start compression from level 2 onwards.
  // this may or may not be optimal
  options.compression_per_level.resize(options.num_levels);
  for (int level = 0; level < options.num_levels; ++level) {
    options.compression_per_level[level] =
        (level >= 2 ? rocksdb::kLZ4Compression : rocksdb::kNoCompression);
  }

  // speed up write performance at the expense of snapshot consistency.
  // this implies that we cannot use snapshots in this instance to get
  // repeatable reads
  options.unordered_write = true;

  // not needed, as all data in this RocksDB instance is ephemeral
  options.avoid_flush_during_shutdown = true;

  // ephemeral data only
  options.paranoid_checks = false;

  // TODO: this configuration may not be optimal. we need to experiment
  // with the settings to see which configuration provides the best
  // performance and least background activity.
  options.max_background_jobs = 2;
  options.max_subcompactions = 2;

  // TODO: later configure write buffer sizes and/or block cache.

  std::vector<rocksdb::ColumnFamilyDescriptor> columnFamilies;

  rocksdb::ColumnFamilyOptions cfOptions;
  cfOptions.force_consistency_checks = false;
  cfOptions.comparator = _comparator.get();
  cfOptions.prefix_extractor.reset(
      rocksdb::NewFixedPrefixTransform(sizeof(uint64_t)));

  rocksdb::BlockBasedTableOptions tableOptions;
  tableOptions.cache_index_and_filter_blocks = true;
  tableOptions.cache_index_and_filter_blocks_with_high_priority = true;
  tableOptions.pin_l0_filter_and_index_blocks_in_cache = true;
  tableOptions.pin_top_level_index_and_filter = true;
  tableOptions.cache_usage_options.options_overrides.insert(
      {rocksdb::CacheEntryRole::kFilterConstruction,
       {/*.charged = */ rocksdb::CacheEntryRoleOptions::Decision::kEnabled}});
  tableOptions.cache_usage_options.options_overrides.insert(
      {rocksdb::CacheEntryRole::kBlockBasedTableReader,
       {/*.charged = */ rocksdb::CacheEntryRoleOptions::Decision::kEnabled}});
  // use 16KB block size as a starting point
  tableOptions.block_size = 16 * 1024;
  tableOptions.checksum = rocksdb::ChecksumType::kxxHash64;
  tableOptions.max_auto_readahead_size = 8 * 1024 * 1024;

  cfOptions.table_factory = std::shared_ptr<rocksdb::TableFactory>(
      rocksdb::NewBlockBasedTableFactory(tableOptions));

  columnFamilies.emplace_back(
      rocksdb::ColumnFamilyDescriptor("SortCF", cfOptions));
  columnFamilies.emplace_back(rocksdb::ColumnFamilyDescriptor(
      rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));

  // path for RocksDB data directory, managed by RocksDB.
  std::string rocksDBPath =
      basics::FileUtils::buildFilename(_basePath, "rocksdb");

  TRI_ASSERT(_db == nullptr);
  rocksdb::Status s = rocksdb::DB::Open(options, rocksDBPath, columnFamilies,
                                        &_cfHandles, &_db);

  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  TRI_ASSERT(_db != nullptr);

  return {};
}

void RocksDBTempStorage::close() {
  if (_db != nullptr) {
    for (auto* handle : _cfHandles) {
      _db->DestroyColumnFamilyHandle(handle);
    }

    _db->Close();

    delete _db;
    _db = nullptr;
  }
}

std::unique_ptr<RocksDBSortedRowsStorageContext>
RocksDBTempStorage::getSortedRowsStorageContext(
    RocksDBMethodsMemoryTracker& memoryTracker) {
  return std::make_unique<RocksDBSortedRowsStorageContext>(
      _db, _cfHandles[0], _tempFilesPath, nextId(), _usageTracker,
      memoryTracker);
}

uint64_t RocksDBTempStorage::nextId() noexcept {
  return _nextId.fetch_add(1, std::memory_order_relaxed) + 1;
}
