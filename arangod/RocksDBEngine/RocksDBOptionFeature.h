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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include "RestServer/arangod.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBOptionsProvider.h"

namespace arangodb {
namespace options {
class ProgramOptions;
}

// This feature is used to configure RocksDB in a central place.
//
// The RocksDB-Storage-Engine and the MMFiles-Persistent-Index
// that are never activated at the same time take options set
// in this feature

class RocksDBOptionFeature final : public ArangodFeature,
                                   public RocksDBOptionsProvider {
 public:
  static constexpr std::string_view name() noexcept { return "RocksDBOption"; }

  explicit RocksDBOptionFeature(Server& server);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;

  rocksdb::TransactionDBOptions getTransactionDBOptions() const override;
  rocksdb::ColumnFamilyOptions getColumnFamilyOptions(
      RocksDBColumnFamilyManager::Family family) const override;

  bool exclusiveWrites() const noexcept { return _exclusiveWrites; }
  bool useFileLogging() const noexcept override { return _useFileLogging; }
  bool limitOpenFilesAtStartup() const noexcept override {
    return _limitOpenFilesAtStartup;
  }
  uint64_t maxTotalWalSize() const noexcept override {
    return _maxTotalWalSize;
  }
  uint32_t numThreadsHigh() const noexcept override { return _numThreadsHigh; }
  uint32_t numThreadsLow() const noexcept override { return _numThreadsLow; }
  uint64_t periodicCompactionTtl() const noexcept override {
    return _periodicCompactionTtl;
  }

 protected:
  rocksdb::Options doGetOptions() const override;
  rocksdb::BlockBasedTableOptions doGetTableOptions() const override;

 private:
  uint64_t _transactionLockStripes;
  int64_t _transactionLockTimeout;
  std::string _walDirectory;
  uint64_t _totalWriteBufferSize;
  uint64_t _writeBufferSize;
  // Update max_write_buffer_number above if you change number of families used
  uint64_t _maxWriteBufferNumber;
  int64_t _maxWriteBufferSizeToMaintain;
  uint64_t _maxTotalWalSize;
  uint64_t _delayedWriteRate;
  uint64_t _minWriteBufferNumberToMerge;
  uint64_t _numLevels;
  uint64_t _numUncompressedLevels;
  uint64_t _maxBytesForLevelBase;
  double _maxBytesForLevelMultiplier;
  int32_t _maxBackgroundJobs;
  uint32_t _maxSubcompactions;
  uint32_t _numThreadsHigh;
  uint32_t _numThreadsLow;
  uint64_t _targetFileSizeBase;
  uint64_t _targetFileSizeMultiplier;
  uint64_t _blockCacheSize;
  int64_t _blockCacheShardBits;
  // only used for HyperClockCache
  uint64_t _blockCacheEstimatedEntryCharge;
  uint64_t _minBlobSize;
  uint64_t _blobFileSize;
  uint32_t _blobFileStartingLevel;
  bool _enableBlobFiles;
  bool _enableBlobCache;
  double _blobGarbageCollectionAgeCutoff;
  double _blobGarbageCollectionForceThreshold;
  double _bloomBitsPerKey;
  uint64_t _tableBlockSize;
  uint64_t _compactionReadaheadSize;
  int64_t _level0CompactionTrigger;
  int64_t _level0SlowdownTrigger;
  int64_t _level0StopTrigger;
  uint64_t _pendingCompactionBytesSlowdownTrigger;
  uint64_t _pendingCompactionBytesStopTrigger;
  uint64_t _periodicCompactionTtl;
  size_t _recycleLogFileNum;
  std::string _compressionType;
  std::string _blobCompressionType;
  std::string _blockCacheType;
  std::string _checksumType;
  std::string _compactionStyle;
  uint32_t _formatVersion;
  bool _optimizeFiltersForMemory;
  bool _enableIndexCompression;
  bool _useJemallocAllocator;
  bool _prepopulateBlockCache;
  bool _prepopulateBlobCache;
  bool _reserveTableBuilderMemory;
  bool _reserveTableReaderMemory;
  bool _reserveFileMetadataMemory;
  bool _enforceBlockCacheSizeLimit;
  bool _cacheIndexAndFilterBlocks;
  bool _cacheIndexAndFilterBlocksWithHighPriority;
  bool _pinl0FilterAndIndexBlocksInCache;
  bool _pinTopLevelIndexAndFilter;
  bool _blockAlignDataBlocks;
  bool _enablePipelinedWrite;
  bool _optimizeFiltersForHits;
  bool _useDirectReads;
  bool _useDirectIoForFlushAndCompaction;
  bool _useFSync;
  bool _skipCorrupted;
  bool _dynamicLevelBytes;
  bool _enableStatistics;
  bool _useFileLogging;
  bool _limitOpenFilesAtStartup;
  bool _allowFAllocate;
  bool _enableBlobGarbageCollection;
  bool _exclusiveWrites;
  bool _minWriteBufferNumberToMergeTouched;
  bool _partitionFilesForDocumentsCf;
  bool _partitionFilesForPrimaryIndexCf;
  bool _partitionFilesForEdgeIndexCf;
  bool _partitionFilesForVPackIndexCf;
  bool _partitionFilesForMdiIndexCf;
  bool _partitionFilesForVectorIndexCf;

  /// per column family write buffer limits
  std::array<uint64_t, RocksDBColumnFamilyManager::numberOfColumnFamilies>
      _maxWriteBufferNumberCf;
};

}  // namespace arangodb
