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

#pragma once

#include "RocksDBEngine/Methods/RocksDBBatchedBaseMethods.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"

#include <rocksdb/sst_file_writer.h>
#include <rocksdb/status.h>

namespace arangodb {
class RocksDBMemoryUsageTracker;
class StorageUsageTracker;

/// wraps an sst file writer - non transactional
class RocksDBSstFileMethods final : public RocksDBBatchedBaseMethods {
 public:
  explicit RocksDBSstFileMethods(bool isForeground, rocksdb::DB* rootDB,
                                 RocksDBTransactionCollection* trxColl,
                                 RocksDBIndex* ridx,
                                 rocksdb::Options const& dbOptions,
                                 std::string const& idxPath,
                                 StorageUsageTracker& usageTracker,
                                 RocksDBMethodsMemoryTracker& memoryTracker);

  explicit RocksDBSstFileMethods(rocksdb::DB* rootDB,
                                 rocksdb::ColumnFamilyHandle* cf,
                                 rocksdb::Options const& dbOptions,
                                 std::string const& idxPath,
                                 StorageUsageTracker& usageTracker,
                                 RocksDBMethodsMemoryTracker& memoryTracker);

  ~RocksDBSstFileMethods();

  rocksdb::Status Get(rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&,
                      rocksdb::PinnableSlice*, ReadOwnWrites) override;
  rocksdb::Status GetForUpdate(rocksdb::ColumnFamilyHandle*,
                               rocksdb::Slice const&,
                               rocksdb::PinnableSlice*) override;
  rocksdb::Status Put(rocksdb::ColumnFamilyHandle*, RocksDBKey const& key,
                      rocksdb::Slice const& val, bool assume_tracked) override;
  rocksdb::Status PutUntracked(rocksdb::ColumnFamilyHandle*,
                               RocksDBKey const& key,
                               rocksdb::Slice const& val) override;
  rocksdb::Status Delete(rocksdb::ColumnFamilyHandle*,
                         RocksDBKey const& key) override;
  rocksdb::Status SingleDelete(rocksdb::ColumnFamilyHandle*,
                               RocksDBKey const&) override;
  void PutLogData(rocksdb::Slice const&) override;

  Result stealFileNames(std::vector<std::string>& fileNames);

  uint64_t stealBytesWrittenToDir() noexcept;

  static void cleanUpFiles(std::vector<std::string> const& fileNames);

 private:
  size_t currentWriteBatchSize() const noexcept override;

  void cleanUpFiles();

  rocksdb::Status writeToFile();
  void insertEstimators();

  static constexpr uint64_t kMaxDataSize = 64 * 1024 * 1024;
  uint64_t _bytesToWriteCount = 0;

  bool _isForeground;
  rocksdb::DB* _rootDB;
  RocksDBTransactionCollection* _trxColl;
  RocksDBIndex* _ridx;
  rocksdb::ColumnFamilyHandle* _cf;
  rocksdb::SstFileWriter _sstFileWriter;
  std::string _idxPath;
  std::vector<std::string> _sstFileNames;
  std::vector<std::pair<std::string, std::string>> _keyValPairs;
  StorageUsageTracker& _usageTracker;
  uint64_t _bytesWrittenToDir;
};

}  // namespace arangodb
