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

#include "RocksDBSstFileMethods.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/files.h"
#include "Basics/FileUtils.h"
#include "Basics/RocksDBUtils.h"
#include "Basics/Thread.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/TemporaryStorageFeature.h"

#include <absl/strings/str_cat.h>

using namespace arangodb;

RocksDBSstFileMethods::RocksDBSstFileMethods(
    bool isForeground, rocksdb::DB* rootDB,
    RocksDBTransactionCollection* trxColl, RocksDBIndex* ridx,
    rocksdb::Options const& dbOptions, std::string const& idxPath,
    StorageUsageTracker& usageTracker,
    RocksDBMethodsMemoryTracker& memoryTracker)
    : RocksDBBatchedBaseMethods(memoryTracker),
      _isForeground(isForeground),
      _rootDB(rootDB),
      _trxColl(trxColl),
      _ridx(ridx),
      _cf(ridx->columnFamily()),
      _sstFileWriter(rocksdb::EnvOptions(dbOptions), dbOptions,
                     ridx->columnFamily()->GetComparator(),
                     ridx->columnFamily()),
      _idxPath(idxPath),
      _usageTracker(usageTracker),
      _bytesWrittenToDir(0) {}

RocksDBSstFileMethods::RocksDBSstFileMethods(
    rocksdb::DB* rootDB, rocksdb::ColumnFamilyHandle* cf,
    rocksdb::Options const& dbOptions, std::string const& idxPath,
    StorageUsageTracker& usageTracker,
    RocksDBMethodsMemoryTracker& memoryTracker)
    : RocksDBBatchedBaseMethods(memoryTracker),
      _isForeground(false),
      _rootDB(rootDB),
      _trxColl(nullptr),
      _ridx(nullptr),
      _cf(cf),
      _sstFileWriter(rocksdb::EnvOptions(dbOptions), dbOptions,
                     _cf->GetComparator(), _cf),
      _idxPath(idxPath),
      _usageTracker(usageTracker),
      _bytesWrittenToDir(0) {}

RocksDBSstFileMethods::~RocksDBSstFileMethods() { cleanUpFiles(); }

void RocksDBSstFileMethods::insertEstimators() {
  auto ops = _trxColl->stealTrackedIndexOperations();
  if (!ops.empty()) {
    TRI_ASSERT(_ridx != nullptr);
    TRI_ASSERT(_ridx->hasSelectivityEstimate() && ops.size() == 1);
    auto it = ops.begin();
    TRI_ASSERT(_ridx->id() == it->first);

    auto* estimator = _ridx->estimator();
    if (estimator != nullptr) {
      if (_isForeground) {
        estimator->insert(it->second.inserts);
      } else {
        uint64_t seq = _rootDB->GetLatestSequenceNumber();
        // since cuckoo estimator uses a map with seq as key we need to
        estimator->bufferUpdates(seq, std::move(it->second.inserts),
                                 std::move(it->second.removals));
      }
    }
  }
}

rocksdb::Status RocksDBSstFileMethods::writeToFile() {
  if (_keyValPairs.empty()) {
    return rocksdb::Status::OK();
  }

  auto comparator = _cf->GetComparator();
  std::sort(_keyValPairs.begin(), _keyValPairs.end(),
            [&comparator](auto const& v1, auto const& v2) {
              return comparator->Compare({v1.first}, {v2.first}) < 0;
            });
  TRI_pid_t pid = Thread::currentProcessId();
  std::string tmpFileName =
      absl::StrCat(pid, "-", RandomGenerator::interval(UINT32_MAX));
  std::string fileName = absl::StrCat(
      basics::FileUtils::buildFilename(_idxPath, tmpFileName), ".sst");
  rocksdb::Status res = _sstFileWriter.Open(fileName);
  if (res.ok()) {
    _bytesToWriteCount = 0;
    _sstFileNames.emplace_back(fileName);
    for (auto const& [key, val] : _keyValPairs) {
      res = _sstFileWriter.Put(rocksdb::Slice(key), rocksdb::Slice(val));
      if (!res.ok()) {
        break;
      }
    }
    _keyValPairs.clear();
    if (res.ok()) {
      uint64_t size = _sstFileWriter.FileSize();
      // track the disk usage for this file. this may throw.
      try {
        _usageTracker.increaseUsage(size);
        _bytesWrittenToDir += size;
        res = _sstFileWriter.Finish();
      } catch (...) {
        cleanUpFiles();
        throw;
      }
    }
    if (!res.ok()) {
      cleanUpFiles();
    } else if (_ridx != nullptr) {
      insertEstimators();
    }
  }

  if (!res.ok()) {
    LOG_TOPIC("fad7f", WARN, Logger::ENGINES)
        << "error during insertion into SST file " << fileName << ": "
        << rocksutils::convertStatus(res).errorMessage();
  }

  return res;
}

Result RocksDBSstFileMethods::stealFileNames(
    std::vector<std::string>& fileNames) {
  rocksdb::Status res = writeToFile();
  if (res.ok()) {
    fileNames = std::move(_sstFileNames);
  }
  return rocksutils::convertStatus(res);
}

uint64_t RocksDBSstFileMethods::stealBytesWrittenToDir() noexcept {
  uint64_t value = _bytesWrittenToDir;
  _bytesWrittenToDir = 0;
  return value;
}

void RocksDBSstFileMethods::cleanUpFiles(
    std::vector<std::string> const& fileNames) {
  for (auto const& fileName : fileNames) {
    TRI_UnlinkFile(fileName.data());
  }
}

void RocksDBSstFileMethods::cleanUpFiles() {
  cleanUpFiles(_sstFileNames);
  _sstFileNames.clear();
  _usageTracker.decreaseUsage(_bytesWrittenToDir);
  _bytesWrittenToDir = 0;
}

rocksdb::Status RocksDBSstFileMethods::Get(rocksdb::ColumnFamilyHandle* cf,
                                           rocksdb::Slice const& key,
                                           rocksdb::PinnableSlice* val,
                                           ReadOwnWrites) {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "SstFileMethods does not provide Get");
}

rocksdb::Status RocksDBSstFileMethods::GetForUpdate(
    rocksdb::ColumnFamilyHandle* cf, rocksdb::Slice const& key,
    rocksdb::PinnableSlice* val) {
  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL, "SstFileMethods does not provide GetForUpdate");
}

rocksdb::Status RocksDBSstFileMethods::Put(rocksdb::ColumnFamilyHandle* cf,
                                           RocksDBKey const& key,
                                           rocksdb::Slice const& val,
                                           bool assume_tracked) {
  TRI_ASSERT(cf != nullptr);
  if (_cf == nullptr) {
    _cf = cf;
  }
  _keyValPairs.emplace_back(std::make_pair(
      key.string().ToString(), std::string(val.data(), val.size())));
  _bytesToWriteCount += key.size() + val.size();
  if (_bytesToWriteCount >= this->kMaxDataSize) {
    return writeToFile();
  }
  return rocksdb::Status::OK();
}

rocksdb::Status RocksDBSstFileMethods::PutUntracked(
    rocksdb::ColumnFamilyHandle* cf, RocksDBKey const& key,
    rocksdb::Slice const& val) {
  return this->Put(cf, key, val, false);
}

rocksdb::Status RocksDBSstFileMethods::Delete(rocksdb::ColumnFamilyHandle* cf,
                                              RocksDBKey const& key) {
  TRI_ASSERT(cf != nullptr);
  TRI_ASSERT(false);
  return rocksdb::Status::NotSupported();
}

rocksdb::Status RocksDBSstFileMethods::SingleDelete(
    rocksdb::ColumnFamilyHandle* cf, RocksDBKey const& key) {
  return this->Delete(cf, key);
}

void RocksDBSstFileMethods::PutLogData(rocksdb::Slice const& blob) {
  TRI_ASSERT(false);
}

size_t RocksDBSstFileMethods::currentWriteBatchSize() const noexcept {
  return _bytesToWriteCount;
}
