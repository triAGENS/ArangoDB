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

#include <algorithm>
#include <cstddef>
#include <ios>
#include <limits>
#include <memory>

#include "RocksDBOptionFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Agency/AgencyFeature.h"
#include "Basics/NumberOfCores.h"
#include "Basics/PhysicalMemory.h"
#include "Basics/application-exit.h"
#include "Basics/process-utils.h"
#include "Basics/system-functions.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/Option.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBPrefixExtractor.h"

#include <rocksdb/advanced_options.h>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/memory_allocator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_partitioner.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>

// It's not atomic because it shouldn't change after initilization.
// And initialization should happen before rocksdb initialization.
static bool ioUringEnabled = true;

// weak symbol from rocksdb
extern "C" bool RocksDbIOUringEnable() { return ioUringEnabled; }

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::options;

namespace {
// compression
std::string const kCompressionTypeSnappy = "snappy";
std::string const kCompressionTypeLZ4 = "lz4";
std::string const kCompressionTypeLZ4HC = "lz4hc";
std::string const kCompressionTypeNone = "none";

std::unordered_set<std::string> const compressionTypes = {
    {kCompressionTypeSnappy},
    {kCompressionTypeLZ4},
    {kCompressionTypeLZ4HC},
    {kCompressionTypeNone}};

rocksdb::CompressionType compressionTypeFromString(std::string_view type) {
  if (type == kCompressionTypeNone) {
    return rocksdb::kNoCompression;
  }
  if (type == kCompressionTypeSnappy) {
    return rocksdb::kSnappyCompression;
  }
  if (type == kCompressionTypeLZ4) {
    return rocksdb::kLZ4Compression;
  }
  if (type == kCompressionTypeLZ4HC) {
    return rocksdb::kLZ4HCCompression;
  }
  TRI_ASSERT(false);
  LOG_TOPIC("edc91", FATAL, arangodb::Logger::STARTUP)
      << "unexpected compression type '" << type << "'";
  FATAL_ERROR_EXIT();
}

// types of block cache
std::string const kBlockCacheTypeLRU = "lru";
std::string const kBlockCacheTypeHyperClock = "hyper-clock";

std::unordered_set<std::string> const blockCacheTypes = {
    {kBlockCacheTypeLRU}, {kBlockCacheTypeHyperClock}};

// checksum types
std::string const kChecksumTypeCRC32C = "crc32c";
std::string const kChecksumTypeXXHash = "xxHash";
std::string const kChecksumTypeXXHash64 = "xxHash64";
std::string const kChecksumTypeXXH3 = "XXH3";

std::unordered_set<std::string> const checksumTypes = {
    kChecksumTypeCRC32C, kChecksumTypeXXHash, kChecksumTypeXXHash64,
    kChecksumTypeXXH3};

// compaction styles
std::string const kCompactionStyleLevel = "level";
std::string const kCompactionStyleUniversal = "universal";
std::string const kCompactionStyleFifo = "fifo";
std::string const kCompactionStyleNone = "none";

std::unordered_set<std::string> const compactionStyles = {
    kCompactionStyleLevel, kCompactionStyleUniversal, kCompactionStyleFifo,
    kCompactionStyleNone};

rocksdb::CompactionStyle compactionStyleFromString(std::string_view type) {
  if (type == kCompactionStyleLevel) {
    return rocksdb::kCompactionStyleLevel;
  }
  if (type == kCompactionStyleUniversal) {
    return rocksdb::kCompactionStyleUniversal;
  }
  if (type == kCompactionStyleFifo) {
    return rocksdb::kCompactionStyleFIFO;
  }
  if (type == kCompactionStyleNone) {
    return rocksdb::kCompactionStyleNone;
  }

  TRI_ASSERT(false);
  LOG_TOPIC("edc92", FATAL, arangodb::Logger::STARTUP)
      << "unexpected compaction style '" << type << "'";
  FATAL_ERROR_EXIT();
}

// defaults
rocksdb::TransactionDBOptions rocksDBTrxDefaults;
rocksdb::Options rocksDBDefaults;
rocksdb::BlockBasedTableOptions rocksDBTableOptionsDefaults;

// minimum size of a block cache shard. we want to at least store
// that much data in each shard (rationale: a data block read from
// disk must fit into the block cache if the block cache's strict
// capacity limit is set. otherwise the block cache will fail reads
// with Status::Incomplete() or Status::MemoryLimit()).
constexpr uint64_t minShardSize = 128 * 1024 * 1024;

uint64_t defaultBlockCacheSize() {
  if (PhysicalMemory::getValue() >= (static_cast<uint64_t>(4) << 30)) {
    // if we have at least 4GB of RAM, the default size is (RAM - 2GB) * 0.3
    return static_cast<uint64_t>(
        (PhysicalMemory::getValue() - (static_cast<uint64_t>(2) << 30)) * 0.3);
  }
  if (PhysicalMemory::getValue() >= (static_cast<uint64_t>(2) << 30)) {
    // if we have at least 2GB of RAM, the default size is 512MB
    return (static_cast<uint64_t>(512) << 20);
  }
  if (PhysicalMemory::getValue() >= (static_cast<uint64_t>(1) << 30)) {
    // if we have at least 1GB of RAM, the default size is 256MB
    return (static_cast<uint64_t>(256) << 20);
  }
  // for everything else the default size is 128MB
  return (static_cast<uint64_t>(128) << 20);
}

uint64_t defaultTotalWriteBufferSize() {
  if (PhysicalMemory::getValue() >= (static_cast<uint64_t>(4) << 30)) {
    // if we have at least 4GB of RAM, the default size is (RAM - 2GB) * 0.4
    return static_cast<uint64_t>(
        (PhysicalMemory::getValue() - (static_cast<uint64_t>(2) << 30)) * 0.4);
  }
  if (PhysicalMemory::getValue() >= (static_cast<uint64_t>(1) << 30)) {
    // if we have at least 1GB of RAM, the default size is 512MB
    return (static_cast<uint64_t>(512) << 20);
  }
  // for everything else the default size is 256MB
  return (static_cast<uint64_t>(256) << 20);
}

uint64_t defaultMinWriteBufferNumberToMerge(uint64_t totalSize,
                                            uint64_t sizePerBuffer,
                                            uint64_t maxBuffers) {
  uint64_t safe = rocksDBDefaults.min_write_buffer_number_to_merge;
  uint64_t test = safe + 1;

  // increase it to as much as 4 if it makes sense
  for (; test <= 4; ++test) {
    // next make sure we have enough buffers for it to matter
    uint64_t minBuffers = 1 + (2 * test);
    if (maxBuffers < minBuffers) {
      break;
    }

    // next make sure we have enough space for all the buffers
    if (minBuffers * sizePerBuffer *
            RocksDBColumnFamilyManager::numberOfColumnFamilies >
        totalSize) {
      break;
    }

    safe = test;
  }

  return safe;
}

}  // namespace

RocksDBOptionFeature::RocksDBOptionFeature(Server& server)
    : ArangodFeature{server, *this},
      // number of lock stripes for the transaction lock manager. we bump this
      // to at least 16 to reduce contention for small scale systems.
      _transactionLockStripes(
          std::max(NumberOfCores::getValue(), std::size_t(16))),
      _transactionLockTimeout(rocksDBTrxDefaults.transaction_lock_timeout),
      _totalWriteBufferSize(rocksDBDefaults.db_write_buffer_size),
      _writeBufferSize(rocksDBDefaults.write_buffer_size),
      _maxWriteBufferNumber(RocksDBColumnFamilyManager::numberOfColumnFamilies +
                            2),  // number of column families plus 2
      _maxWriteBufferSizeToMaintain(0),
      _maxTotalWalSize(256 << 20),
      _delayedWriteRate(rocksDBDefaults.delayed_write_rate),
      _minWriteBufferNumberToMerge(defaultMinWriteBufferNumberToMerge(
          _totalWriteBufferSize, _writeBufferSize, _maxWriteBufferNumber)),
      _numLevels(rocksDBDefaults.num_levels),
      _numUncompressedLevels(2),
      _maxBytesForLevelBase(rocksDBDefaults.max_bytes_for_level_base),
      _maxBytesForLevelMultiplier(
          rocksDBDefaults.max_bytes_for_level_multiplier),
      // set max number of background jobs to the number of available cores
      _maxBackgroundJobs(static_cast<int32_t>(
          std::max(static_cast<size_t>(2), NumberOfCores::getValue()))),
      _maxSubcompactions(4),
      _numThreadsHigh(0),
      _numThreadsLow(0),
      _targetFileSizeBase(rocksDBDefaults.target_file_size_base),
      _targetFileSizeMultiplier(rocksDBDefaults.target_file_size_multiplier),
      _blockCacheSize(::defaultBlockCacheSize()),
      _blockCacheShardBits(-1),
      _blockCacheEstimatedEntryCharge(0),
      _minBlobSize(256),
      _blobFileSize(1ULL << 30),
      _blobFileStartingLevel(0),
      _enableBlobFiles(false),
      _enableBlobCache(false),
      _blobGarbageCollectionAgeCutoff(0.25),
      _blobGarbageCollectionForceThreshold(1.0),
      _bloomBitsPerKey(10.0),
      _tableBlockSize(std::max(
          rocksDBTableOptionsDefaults.block_size,
          static_cast<decltype(rocksDBTableOptionsDefaults.block_size)>(16 *
                                                                        1024))),
      _compactionReadaheadSize(8 * 1024 * 1024),
      _level0CompactionTrigger(2),
      _level0SlowdownTrigger(16),
      _level0StopTrigger(256),
      // pending compactions slowdown trigger is set to 1GB
      _pendingCompactionBytesSlowdownTrigger(1024ULL * 1024ULL * 1024ULL),
      // pending compactions stop trigger is set to 32GB
      _pendingCompactionBytesStopTrigger(32ULL * 1024ULL * 1024ULL * 1024ULL),
      // note: this is a default value from RocksDB (db/column_family.cc,
      // kAdjustedTtl):
      _periodicCompactionTtl(30 * 24 * 60 * 60),
      _recycleLogFileNum(0),
      _compressionType(::kCompressionTypeLZ4),
      _blobCompressionType(::kCompressionTypeLZ4),
      _blockCacheType(::kBlockCacheTypeLRU),
      _checksumType(::kChecksumTypeXXHash64),
      _compactionStyle(::kCompactionStyleLevel),
      // stay at format version 5 for now so downgrading back to the older
      // RocksDB 7.2 is possible with the datafiles from the newer version
      // of RocksDB. Later go to format version 6
      _formatVersion(5),
      // note: the following option has historically had a default value of
      // false in RocksDB. RocksDB 9.1 changes the default value to true.
      // explicitly set it to false here to keep old behavior intact
      _optimizeFiltersForMemory(false),
      _enableIndexCompression(
          rocksDBTableOptionsDefaults.enable_index_compression),
      _useJemallocAllocator(false),
      _prepopulateBlockCache(false),
      _prepopulateBlobCache(false),
      _reserveTableBuilderMemory(true),
      _reserveTableReaderMemory(true),
      _reserveFileMetadataMemory(true),
      _enforceBlockCacheSizeLimit(false),
      _cacheIndexAndFilterBlocks(true),
      _cacheIndexAndFilterBlocksWithHighPriority(
          rocksDBTableOptionsDefaults
              .cache_index_and_filter_blocks_with_high_priority),
      _pinl0FilterAndIndexBlocksInCache(
          rocksDBTableOptionsDefaults.pin_l0_filter_and_index_blocks_in_cache),
      _pinTopLevelIndexAndFilter(
          rocksDBTableOptionsDefaults.pin_top_level_index_and_filter),
      _blockAlignDataBlocks(rocksDBTableOptionsDefaults.block_align),
      _enablePipelinedWrite(true),
      _optimizeFiltersForHits(rocksDBDefaults.optimize_filters_for_hits),
      _useDirectReads(rocksDBDefaults.use_direct_reads),
      _useDirectIoForFlushAndCompaction(
          rocksDBDefaults.use_direct_io_for_flush_and_compaction),
      _useFSync(rocksDBDefaults.use_fsync),
      _skipCorrupted(false),
      _dynamicLevelBytes(true),
      _enableStatistics(false),
      _useFileLogging(false),
      _limitOpenFilesAtStartup(false),
      _allowFAllocate(true),
      _enableBlobGarbageCollection(true),
      _exclusiveWrites(false),
      _minWriteBufferNumberToMergeTouched(false),
      _partitionFilesForDocumentsCf(true),
      _partitionFilesForPrimaryIndexCf(false),
      _partitionFilesForEdgeIndexCf(false),
      _partitionFilesForVPackIndexCf(false),
      _partitionFilesForMdiIndexCf(false),
      _partitionFilesForVectorIndexCf(false),
      _maxWriteBufferNumberCf{0, 0, 0, 0, 0, 0, 0, 0, 0, 0} {
  if (_totalWriteBufferSize == 0) {
    // unlimited write buffer size... now set to some fraction of physical RAM
    _totalWriteBufferSize = ::defaultTotalWriteBufferSize();
  }

  setOptional(true);
  startsAfter<BasicFeaturePhaseServer>();
}

void RocksDBOptionFeature::collectOptions(
    std::shared_ptr<ProgramOptions> options) {
  options->addSection("rocksdb", "RocksDB engine");

  options->addObsoleteOption("--rocksdb.enabled",
                             "Whether the RocksDB engine is enabled for the "
                             "persistent index type - this option is obsolete "
                             "and always active!",
                             true);

  options->addOption("--rocksdb.wal-directory",
                     "Absolute path for RocksDB WAL files. If not set, a "
                     "subdirectory `journals` inside the database directory "
                     "is used.",
                     new StringParameter(&_walDirectory),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption("--rocksdb.target-file-size-base",
                     "Per-file target file size for compaction (in bytes). The "
                     "actual target file size for each level is "
                     "`--rocksdb.target-file-size-base` multiplied by "
                     "`--rocksdb.target-file-size-multiplier` ^ (level - 1)",
                     new UInt64Parameter(&_targetFileSizeBase),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.target-file-size-multiplier",
      "The multiplier for `--rocksdb.target-file-size`. A value of 1 means "
      "that files in different levels will have the same size.",
      new UInt64Parameter(&_targetFileSizeMultiplier, /*base*/ 1,
                          /*minValue*/ 1),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  TRI_ASSERT(::compressionTypes.contains(_compressionType));
  options
      ->addOption("--rocksdb.compression-type",
                  "The compression algorithm to use within RocksDB.",
                  new DiscreteValuesParameter<StringParameter>(
                      &_compressionType, ::compressionTypes))
      .setIntroducedIn(31000);

  options
      ->addOption("--rocksdb.transaction-lock-stripes",
                  "The number of lock stripes to use for transaction locks.",
                  new UInt64Parameter(&_transactionLockStripes),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Dynamic,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30902)
      .setLongDescription(R"(You can control the number of lock stripes to use
for RocksDB's transaction lock manager with this option. You can use higher
values to reduce a potential contention in the lock manager.

The option defaults to the number of available cores, but is increased to a
value of `16` if the number of cores is lower.)");

  options->addOption(
      "--rocksdb.transaction-lock-timeout",
      "If positive, specifies the wait timeout in milliseconds when "
      " a transaction attempts to lock a document. A negative value "
      "is not recommended as it can lead to deadlocks (0 = no waiting, < 0 no "
      "timeout)",
      new Int64Parameter(&_transactionLockTimeout),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options
      ->addOption(
          "--rocksdb.total-write-buffer-size",
          "The maximum total size of in-memory write buffers (0 = unbounded).",
          new UInt64Parameter(&_totalWriteBufferSize),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Dynamic,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The total amount of data to build up in all
in-memory buffers (backed by log files). You can use this option together with
the block cache size configuration option to limit memory usage.

If set to `0`, the memory usage is not limited.

If set to a value larger than `0`, this caps memory usage for write buffers but
may have an effect on performance. If there is more than 4 GiB of RAM in the
system, the default value is `(system RAM size - 2 GiB) * 0.5`.

For systems with less RAM, the default values are:

- 512 MiB for systems with between 1 and 4 GiB of RAM.
- 256 MiB for systems with less than 1 GiB of RAM.)");

  options
      ->addOption("--rocksdb.write-buffer-size",
                  "The amount of data to build up in memory before "
                  "converting to a sorted on-disk file (0 = disabled).",
                  new UInt64Parameter(&_writeBufferSize),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The amount of data to build up in each in-memory
buffer (backed by a log file) before closing the buffer and queuing it to be
flushed to standard storage. Larger values than the default may improve
performance, especially for bulk loads.)");

  options
      ->addOption(
          "--rocksdb.max-write-buffer-number",
          "The maximum number of write buffers that build up in memory "
          "(default: number of column families + 2 = 12 write buffers). "
          "You can only increase the number.",
          new UInt64Parameter(&_maxWriteBufferNumber),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(If this number is reached before the buffers can
be flushed, writes are slowed or stalled.)");

  options
      ->addOption(
          "--rocksdb.max-write-buffer-size-to-maintain",
          "The maximum size of immutable write buffers that build up in memory "
          "per column family. Larger values mean that more in-memory data "
          "can be used for transaction conflict checking (-1 = use automatic "
          "default value, 0 = do not keep immutable flushed write buffers, "
          "which is the default and usually correct).",
          new Int64Parameter(&_maxWriteBufferSizeToMaintain),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The default value `0` restores the memory usage
pattern of version 3.6. This makes RocksDB not keep any flushed immutable
write-buffers in memory.)");

  options
      ->addOption("--rocksdb.max-total-wal-size",
                  "The maximum total size of WAL files that force a flush "
                  "of stale column families.",
                  new UInt64Parameter(&_maxTotalWalSize),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(When reached, force a flush of all column families
whose data is backed by the oldest WAL files. If you set this option to a low
value, regular flushing of column family data from memtables is triggered, so
that WAL files can be moved to the archive.

If you set this option to a high value, regular flushing is avoided but may
prevent WAL files from being moved to the archive and being removed.)");

  options->addOption(
      "--rocksdb.delayed-write-rate",
      "Limit the write rate to the database (in bytes per second) when writing "
      "to the last mem-table allowed and if more than 3 mem-tables are "
      "allowed, or if a certain number of level-0 files are surpassed and "
      "writes need to be slowed down.",
      new UInt64Parameter(&_delayedWriteRate),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options->addOldOption("rocksdb.delayed_write_rate",
                        "rocksdb.delayed-write-rate");

  options->addOption("--rocksdb.min-write-buffer-number-to-merge",
                     "The minimum number of write buffers that are merged "
                     "together before writing to storage.",
                     new UInt64Parameter(&_minWriteBufferNumberToMerge),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::Dynamic,
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption("--rocksdb.num-levels",
                     "The number of levels for the database in the LSM tree.",
                     new UInt64Parameter(&_numLevels, /*base*/ 1,
                                         /*minValue*/ 1, /*maxValue*/ 20),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options
      ->addOption("--rocksdb.num-uncompressed-levels",
                  "The number of levels that do not use compression in the "
                  "LSM tree.",
                  new UInt64Parameter(&_numUncompressedLevels),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Levels above the default of `2` use
compression to reduce the disk space requirements for storing data in these
levels.)");

  options
      ->addOption("--rocksdb.dynamic-level-bytes",
                  "Whether to determine the number of bytes for each level "
                  "dynamically to minimize space amplification.",
                  new BooleanParameter(&_dynamicLevelBytes),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(If set to `true`, the amount of data in each level
of the LSM tree is determined dynamically to minimize the space amplification.
Otherwise, the level sizes are fixed. The dynamic sizing allows RocksDB to
maintain a well-structured LSM tree regardless of total data size.)");

  options->addOption("--rocksdb.max-bytes-for-level-base",
                     "If not using dynamic level sizes, this controls the "
                     "maximum total data size for level-1 of the LSM tree.",
                     new UInt64Parameter(&_maxBytesForLevelBase),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.max-bytes-for-level-multiplier",
      "If not using dynamic level sizes, the maximum number of "
      "bytes for level L of the LSM tree can be calculated as "
      " max-bytes-for-level-base * "
      "(max-bytes-for-level-multiplier ^ (L-1))",
      new DoubleParameter(&_maxBytesForLevelMultiplier, /*base*/ 1.0,
                          /*minValue*/ 0.0,
                          /*maxValue*/ std::numeric_limits<double>::max(),
                          /*minInclusive*/ false),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options
      ->addOption(
          "--rocksdb.block-align-data-blocks",
          "If enabled, data blocks are aligned on the lesser of page size and "
          "block size.",
          new BooleanParameter(&_blockAlignDataBlocks),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(This may waste some memory but may reduce the
number of cross-page I/O operations.)");

  options->addOption("--rocksdb.enable-pipelined-write",
                     "If enabled, use a two stage write queue for WAL writes "
                     "and memtable writes.",
                     new BooleanParameter(&_enablePipelinedWrite),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption("--rocksdb.enable-statistics",
                     "Whether RocksDB statistics should be enabled.",
                     new BooleanParameter(&_enableStatistics),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.optimize-filters-for-hits",
      "Whether the implementation should optimize the filters mainly for cases "
      "where keys are found rather than also optimize for keys missed. You can "
      "enable the option if you know that there are very few misses or the "
      "performance in the case of misses is not important for your "
      "application.",
      new BooleanParameter(&_optimizeFiltersForHits),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.use-direct-reads", "Use O_DIRECT for reading files.",
      new BooleanParameter(&_useDirectReads),
      arangodb::options::makeFlags(arangodb::options::Flags::Uncommon));

  options->addOption(
      "--rocksdb.use-direct-io-for-flush-and-compaction",
      "Use O_DIRECT for writing files for flush and compaction.",
      new BooleanParameter(&_useDirectIoForFlushAndCompaction),
      arangodb::options::makeFlags(arangodb::options::Flags::Uncommon));

  options->addOption(
      "--rocksdb.use-fsync",
      "Whether to use fsync calls when writing to disk (set to false "
      "for issuing fdatasync calls only).",
      new BooleanParameter(&_useFSync),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options
      ->addOption("--rocksdb.max-background-jobs",
                  "The maximum number of concurrent background jobs "
                  "(compactions and flushes).",
                  new Int32Parameter(&_maxBackgroundJobs),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Dynamic,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The jobs are submitted to the low priority thread
pool. The default value is the number of processors in the system.)");

  options->addOption("--rocksdb.max-subcompactions",
                     "The maximum number of concurrent sub-jobs for a "
                     "background compaction.",
                     new UInt32Parameter(&_maxSubcompactions),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options
      ->addOption("--rocksdb.level0-compaction-trigger",
                  "The number of level-0 files that triggers a compaction.",
                  new Int64Parameter(&_level0CompactionTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Compaction of level-0 to level-1 is triggered when
this many files exist in level-0. If you set this option to a higher number, it
may help bulk writes at the expense of slowing down reads.)");

  options
      ->addOption("--rocksdb.level0-slowdown-trigger",
                  "The number of level-0 files that triggers a write slowdown",
                  new Int64Parameter(&_level0SlowdownTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(When this many files accumulate in level-0, writes
are slowed down to `--rocksdb.delayed-write-rate` to allow compaction to
catch up.)");

  options
      ->addOption("--rocksdb.level0-stop-trigger",
                  "The number of level-0 files that triggers a full write stop",
                  new Int64Parameter(&_level0StopTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(When this many files accumulate in level-0, writes
are stopped to allow compaction to catch up.)");

  options
      ->addOption("--rocksdb.pending-compactions-slowdown-trigger",
                  "The number of pending compaction bytes that triggers a "
                  "write slowdown.",
                  new UInt64Parameter(&_pendingCompactionBytesSlowdownTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30805);

  options
      ->addOption("--rocksdb.pending-compactions-stop-trigger",
                  "The number of pending compaction bytes that triggers a full "
                  "write stop.",
                  new UInt64Parameter(&_pendingCompactionBytesStopTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30805);

  options
      ->addOption(
          "--rocksdb.num-threads-priority-high",
          "The number of threads for high priority operations (e.g. flush).",
          new UInt32Parameter(&_numThreadsHigh, /*base*/ 1, /*minValue*/ 0,
                              /*maxValue*/ 64),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30805)
      .setLongDescription(R"(The recommended value is to set this equal to
`max-background-flushes`. The default value is `number of processors / 2`.)");

  options
      ->addOption("--rocksdb.block-cache-estimated-entry-charge",
                  "The estimated charge of cache entries (in bytes) for the "
                  "hyper-clock cache.",
                  new UInt64Parameter(&_blockCacheEstimatedEntryCharge),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31202);

  TRI_ASSERT(::blockCacheTypes.contains(_blockCacheType));
  options
      ->addOption("--rocksdb.block-cache-type",
                  "The block cache type to use (note: the 'hyper-clock' cache "
                  "type is experimental).",
                  new DiscreteValuesParameter<StringParameter>(
                      &_blockCacheType, ::blockCacheTypes))
      .setIntroducedIn(31202);

  options
      ->addOption(
          "--rocksdb.num-threads-priority-low",
          "The number of threads for low priority operations (e.g. "
          "compaction).",
          new UInt32Parameter(&_numThreadsLow, /*base*/ 1, /*minValue*/ 0,
                              /*maxValue*/ 256),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The default value is
`number of processors / 2`.)");

  options
      ->addOption("--rocksdb.block-cache-size",
                  "The size of block cache (in bytes).",
                  new UInt64Parameter(&_blockCacheSize),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Dynamic,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(This is the maximum size of the block cache in
bytes. Increasing this value may improve performance. If there is more than
4 GiB of RAM in the system, the default value is
`(system RAM size - 2GiB) * 0.3`.

For systems with less RAM, the default values are:

- 512 MiB for systems with between 2 and 4 GiB of RAM.
- 256 MiB for systems with between 1 and 2 GiB of RAM.
- 128 MiB for systems with less than 1 GiB of RAM.)");

  options
      ->addOption(
          "--rocksdb.block-cache-shard-bits",
          "The number of shard bits to use for the block cache "
          "(-1 = default value).",
          new Int64Parameter(&_blockCacheShardBits, /*base*/ 1, /*minValue*/ -1,
                             /*maxValue*/ 20, /*minInclusive*/ true,
                             /*maxInclusive*/ false),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(The number of bits used to shard the block cache
to allow concurrent operations. To keep individual shards at a reasonable size
(i.e. at least 512 KiB), keep this value to at most
`block-cache-shard-bits / 512 KiB`. Default: `block-cache-size / 2^19`.)");

  options
      ->addOption("--rocksdb.enforce-block-cache-size-limit",
                  "If enabled, strictly enforces the block cache size limit.",
                  new BooleanParameter(&_enforceBlockCacheSizeLimit),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Whether the maximum size of the RocksDB block
cache is strictly enforced. You can set this option to limit the memory usage of
the block cache to at most the specified size. If inserting a data block into
the cache would exceed the cache's capacity, the data block is not inserted.
If disabled, a data block may still get inserted into the cache. It is evicted
later, but the cache may temporarily grow beyond its capacity limit. 

The default value for `--rocksdb.enforce-block-cache-size-limit` was `false`
before version 3.10, but was changed to `true` from version 3.10 onwards.

To improve stability of memory usage and prevent exceeding the block cache
capacity limit (as configurable via `--rocksdb.block-cache-size`), it is
recommended to set this option to `true`.)");

  options
      ->addOption("--rocksdb.cache-index-and-filter-blocks",
                  "If enabled, the RocksDB block cache quota also includes "
                  "RocksDB memtable sizes.",
                  new BooleanParameter(&_cacheIndexAndFilterBlocks),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(If you set this option to `true`, RocksDB tracks
all loaded index and filter blocks in the block cache, so that they count
towards RocksDB's block cache memory limit.

If you set this option to `false`, the memory usage for index and filter blocks
is not accounted for.

The default value of `--rocksdb.cache-index-and-filter-blocks` was `false` in 
versions before 3.10, and was changed to `true` from version 3.10 onwards.

To improve stability of memory usage and avoid untracked memory allocations by
RocksDB, it is recommended to set this option to `true`. Note that tracking
index and filter blocks leaves less room for other data in the block cache, so
in case servers have unused RAM capacity available, it may be useful to increase
the overall size of the block cache.)");

  options->addOption(
      "--rocksdb.cache-index-and-filter-blocks-with-high-priority",
      "If enabled and `--rocksdb.cache-index-and-filter-blocks` is also "
      "enabled, cache index and filter blocks with high priority, "
      "making index and filter blocks be less likely to be evicted than "
      "data blocks.",
      new BooleanParameter(&_cacheIndexAndFilterBlocksWithHighPriority),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.pin-l0-filter-and-index-blocks-in-cache",
      "If enabled and `--rocksdb.cache-index-and-filter-blocks` is also "
      "enabled, filter and index blocks are pinned and only evicted from "
      "cache when the table reader is freed.",
      new BooleanParameter(&_pinl0FilterAndIndexBlocksInCache),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.pin-top-level-index-and-filter",
      "If enabled and `--rocksdb.cache-index-and-filter-blocks` is also "
      "enabled, the top-level index of partitioned filter and index blocks "
      "are pinned and only evicted from cache when the table reader is "
      "freed.",
      new BooleanParameter(&_pinTopLevelIndexAndFilter),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options->addOption("--rocksdb.table-block-size",
                     "The approximate size (in bytes) of the user data packed "
                     "per block for uncompressed data.",
                     new UInt64Parameter(&_tableBlockSize),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::Uncommon,
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption(
      "--rocksdb.recycle-log-file-num",
      "If enabled, keep a pool of log files around for recycling.",
      new SizeTParameter(&_recycleLogFileNum),
      arangodb::options::makeFlags(
          arangodb::options::Flags::Uncommon,
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options
      ->addOption(
          "--rocksdb.bloom-filter-bits-per-key",
          "The average number of bits to use per key in a Bloom filter.",
          new DoubleParameter(&_bloomBitsPerKey),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Uncommon,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31003);

  options->addOption(
      "--rocksdb.compaction-read-ahead-size",
      "If non-zero, bigger reads are performed when doing compaction. If you "
      "run RocksDB on spinning disks, you should set this to at least 2 MB. "
      "That way, RocksDB's compaction does sequential instead of random reads.",
      new UInt64Parameter(&_compactionReadaheadSize),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnAgent,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle));

  options
      ->addOption("--rocksdb.use-file-logging",
                  "Use a file-base logger for RocksDB's own logs.",
                  new BooleanParameter(&_useFileLogging),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(If set to `true`, enables writing of RocksDB's own
informational log files into RocksDB's database directory.

This option is turned off by default, but you can enable it for debugging
RocksDB internals and performance.)");

  options->addOption("--rocksdb.wal-recovery-skip-corrupted",
                     "Skip corrupted records in WAL recovery.",
                     new BooleanParameter(&_skipCorrupted),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::Uncommon,
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options->addOption("--rocksdb.limit-open-files-at-startup",
                     "Limit the amount of .sst files RocksDB inspects at "
                     "startup, in order to reduce the startup I/O operations.",
                     new BooleanParameter(&_limitOpenFilesAtStartup),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::Uncommon,
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnAgent,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle));

  options
      ->addOption("--rocksdb.allow-fallocate",
                  "Whether to allow RocksDB to use fallocate calls. "
                  "If disabled, fallocate calls are bypassed and no "
                  "pre-allocation is done.",
                  new BooleanParameter(&_allowFAllocate),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Preallocation is turned on by default, but you can
turn it off for operating system versions that are known to have issues with it.
This option only has an effect on operating systems that support
`fallocate`.)");

  options
      ->addOption(
          "--rocksdb.exclusive-writes",
          "If enabled, writes are exclusive. This allows the RocksDB engine to "
          "mimic the collection locking behavior of the now-removed MMFiles "
          "storage engine, but inhibits concurrent write operations.",
          new BooleanParameter(&_exclusiveWrites),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Uncommon,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setDeprecatedIn(30800)
      .setLongDescription(R"(This option allows you to make all writes to the
RocksDB storage exclusive and therefore avoid write-write conflicts.

This option was introduced to open a way to upgrade from the legacy MMFiles to
the RocksDB storage engine without modifying client application code.
You should avoid enabling this option as the use of exclusive locks on
collections introduce a noticeable throughput penalty.

**Note**: The MMFiles engine was removed and this option is a stopgap measure
only. This option is thus deprecated, and will be removed in a future
version.)");

  TRI_ASSERT(::checksumTypes.contains(_checksumType));
  options
      ->addOption("--rocksdb.checksum-type",
                  "The checksum type to use for table files.",
                  new DiscreteValuesParameter<StringParameter>(&_checksumType,
                                                               ::checksumTypes),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  TRI_ASSERT(::compactionStyles.contains(_compactionStyle));
  options
      ->addOption(
          "--rocksdb.compaction-style",
          "The compaction style which is used to pick the next file(s) to "
          "be compacted (note: all styles except 'level' are experimental).",
          new DiscreteValuesParameter<StringParameter>(&_compactionStyle,
                                                       ::compactionStyles),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  std::unordered_set<uint32_t> formatVersions = {3, 4, 5, 6};
  options
      ->addOption("--rocksdb.format-version",
                  "The table format version to use inside RocksDB.",
                  new DiscreteValuesParameter<UInt32Parameter>(&_formatVersion,
                                                               formatVersions),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000)
      .setLongDescription(
          R"(Note that format version 6 can only be read by RocksDB versions
>= 8.6.0. Thus switching to format version 6 will make the database files
incompatible with ArangoDB versions with a lower RocksDB version in case of
downgrading.)");

  options
      ->addOption("--rocksdb.optimize-filters-for-memory",
                  "Optimize RocksDB bloom filters to reduce internal memory "
                  "fragmentation.",
                  new BooleanParameter(&_optimizeFiltersForMemory),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31202);

  options
      ->addOption("--rocksdb.enable-index-compression",
                  "Enable index compression.",
                  new BooleanParameter(&_enableIndexCompression),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  options
      ->addOption("--rocksdb.enable-blob-files",
                  "Enable blob files for the documents column family.",
                  new BooleanParameter(&_enableBlobFiles),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption(
          "--rocksdb.enable-blob-cache",
          "Enable caching of blobs in the block cache for the documents "
          "column family.",
          new BooleanParameter(&_enableBlobCache),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Experimental,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31202);

  options
      ->addOption("--rocksdb.min-blob-size",
                  "The size threshold for storing documents in blob files (in "
                  "bytes, 0 = store all documents in blob files). "
                  "Requires `--rocks.enable-blob-files`.",
                  new UInt64Parameter(&_minBlobSize),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption("--rocksdb.blob-file-size",
                  "The size limit for blob files in the documents column "
                  "family (in bytes). Requires `--rocksdb.enable-blob-files`.",
                  new UInt64Parameter(&_blobFileSize),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption("--rocksdb.blob-file-starting-level",
                  "The level from which on to use blob files in the documents "
                  "column family. Requires `--rocksdb.enable-blob-files`.",
                  new UInt32Parameter(&_blobFileStartingLevel),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31202);

  options
      ->addOption(
          "--rocksdb.blob-garbage-collection-age-cutoff",
          "The age cutoff for garbage collecting blob files in the documents "
          "column family (percentage value from 0 to 1 determines how many "
          "blob files are garbage collected during compaction). Requires "
          "`--rocksdb.enable-blob-files` and "
          "`--rocksdb.enable-blob-garbage-collection`.",
          new DoubleParameter(&_blobGarbageCollectionAgeCutoff, 1.0, 0.0, 1.0),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Experimental,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption(
          "--rocksdb.blob-garbage-collection-force-threshold",
          "The garbage ratio threshold for scheduling targeted compactions "
          "for the oldest blob files in the documents column family "
          "(percentage value between 0 and 1). "
          "Requires `--rocksdb.enable-blob-files` and "
          "`--rocksdb.enable-blob-garbage-collection`.",
          new DoubleParameter(&_blobGarbageCollectionForceThreshold, 1.0, 0.0,
                              1.0),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Experimental,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  TRI_ASSERT(::compressionTypes.contains(_blobCompressionType));
  options
      ->addOption("--rocksdb.blob-compression-type",
                  "The compression algorithm to use for blob data in the "
                  "documents column family. "
                  "Requires `--rocksdb.enable-blob-files`.",
                  new DiscreteValuesParameter<StringParameter>(
                      &_blobCompressionType, ::compressionTypes),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption(
          "--rocksdb.enable-blob-garbage-collection",
          "Enable blob garbage collection during compaction in the "
          "documents column family. Requires `--rocksdb.enable-blob-files`.",
          new BooleanParameter(&_enableBlobGarbageCollection),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Experimental,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption("--rocksdb.prepopulate-blob-cache",
                  "Pre-populate the blob cache on flushes.",
                  new BooleanParameter(&_prepopulateBlobCache),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31202);

  options
      ->addOption(
          "--rocksdb.block-cache-jemalloc-allocator",
          "Use jemalloc-based memory allocator for RocksDB block cache.",
          new BooleanParameter(&_useJemallocAllocator),
          arangodb::options::makeFlags(arangodb::options::Flags::Experimental,
                                       arangodb::options::Flags::Uncommon,
                                       arangodb::options::Flags::OnAgent,
                                       arangodb::options::Flags::OnDBServer,
                                       arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100)
      .setLongDescription(
          R"(The jemalloc-based memory allocator for the RocksDB block cache
will also exclude the block cache contents from coredumps, potentially making generated 
coredumps a lot smaller.
In order to use this option, the executable needs to be compiled with jemalloc
support (which is the default on Linux).)");

  options
      ->addOption("--rocksdb.prepopulate-block-cache",
                  "Pre-populate block cache on flushes.",
                  new BooleanParameter(&_prepopulateBlockCache),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  options
      ->addOption("--rocksdb.reserve-table-builder-memory",
                  "Account for table building memory in block cache.",
                  new BooleanParameter(&_reserveTableBuilderMemory),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  options
      ->addOption("--rocksdb.reserve-table-reader-memory",
                  "Account for table reader memory in block cache.",
                  new BooleanParameter(&_reserveTableReaderMemory),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31000);

  options
      ->addOption("--rocksdb.reserve-file-metadata-memory",
                  "account for .sst file metadata memory in block cache",
                  new BooleanParameter(&_reserveFileMetadataMemory),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption("--rocksdb.periodic-compaction-ttl",
                  "Time-to-live (in seconds) for periodic compaction of .sst "
                  "files, based on the file age (0 = no periodic compaction).",
                  new UInt64Parameter(&_periodicCompactionTtl),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30903)
      .setLongDescription(R"(The default value from RocksDB is ~30 days. To
avoid periodic auto-compaction and the I/O caused by it, you can set this
option to `0`.)");

  options
      ->addOption("--rocksdb.partition-files-for-documents",
                  "If enabled, the document data for different "
                  "collections/shards will end up in "
                  "different .sst files.",
                  new BooleanParameter(&_partitionFilesForDocumentsCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(Enabling this option will make RocksDB's
compaction write the document data for different collections/shards
into different .sst files. Otherwise the document data from different 
collections/shards can be mixed and written into the same .sst files.

Enabling this option usually has the benefit of making the RocksDB
compaction more efficient when a lot of different collections/shards
are written to in parallel.
The disavantage of enabling this option is that there can be more .sst
files than when the option is turned off, and the disk space used by
these .sst files can be higher than if there are fewer .sst files (this
is because there is some per-.sst file overhead).
In particular on deployments with many collections/shards
this can lead to a very high number of .sst files, with the potential
of outgrowing the maximum number of file descriptors the ArangoDB process 
can open. Thus the option should only be enabled on deployments with a
limited number of collections/shards.)");

  options
      ->addOption("--rocksdb.partition-files-for-primary-index",
                  "If enabled, the primary index data for different "
                  "collections/shards will end up in "
                  "different .sst files.",
                  new BooleanParameter(&_partitionFilesForPrimaryIndexCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(Enabling this option will make RocksDB's
compaction write the primary index data for different collections/shards
into different .sst files. Otherwise the primary index data from different 
collections/shards can be mixed and written into the same .sst files.

Enabling this option usually has the benefit of making the RocksDB
compaction more efficient when a lot of different collections/shards
are written to in parallel.
The disavantage of enabling this option is that there can be more .sst
files than when the option is turned off, and the disk space used by
these .sst files can be higher than if there are fewer .sst files (this
is because there is some per-.sst file overhead).
In particular on deployments with many collections/shards
this can lead to a very high number of .sst files, with the potential
of outgrowing the maximum number of file descriptors the ArangoDB process 
can open. Thus the option should only be enabled on deployments with a
limited number of collections/shards.)");

  options
      ->addOption("--rocksdb.partition-files-for-edge-index",
                  "If enabled, the index data for different edge "
                  "indexes will end up in different .sst files.",
                  new BooleanParameter(&_partitionFilesForEdgeIndexCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(Enabling this option will make RocksDB's
compaction write the edge index data for different edge collections/shards
into different .sst files. Otherwise the edge index data from different 
edge collections/shards can be mixed and written into the same .sst files.

Enabling this option usually has the benefit of making the RocksDB
compaction more efficient when a lot of different edge collections/shards
are written to in parallel.
The disavantage of enabling this option is that there can be more .sst
files than when the option is turned off, and the disk space used by
these .sst files can be higher than if there are fewer .sst files (this
is because there is some per-.sst file overhead).
In particular on deployments with many edge collections/shards
this can lead to a very high number of .sst files, with the potential
of outgrowing the maximum number of file descriptors the ArangoDB process 
can open. Thus the option should only be enabled on deployments with a
limited number of edge collections/shards.)");

  options
      ->addOption("--rocksdb.partition-files-for-persistent-index",
                  "If enabled, the index data for different persistent "
                  "indexes will end up in different .sst files.",
                  new BooleanParameter(&_partitionFilesForVPackIndexCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(Enabling this option will make RocksDB's
compaction write the persistent index data for different persistent
indexes (also indexes from different collections/shards) into different 
.sst files. Otherwise the persistent index data from different 
collections/shards/indexes can be mixed and written into the same .sst files.

Enabling this option usually has the benefit of making the RocksDB
compaction more efficient when a lot of different collections/shards/indexes
are written to in parallel.
The disavantage of enabling this option is that there can be more .sst
files than when the option is turned off, and the disk space used by
these .sst files can be higher than if there are fewer .sst files (this
is because there is some per-.sst file overhead).
In particular on deployments with many collections/shards/indexes
this can lead to a very high number of .sst files, with the potential
of outgrowing the maximum number of file descriptors the ArangoDB process 
can open. Thus the option should only be enabled on deployments with a
limited number of edge collections/shards/indexes.)");

  options
      ->addOption("--rocksdb.partition-files-for-mdi-index",
                  "If enabled, the index data for different mdi "
                  "indexes will end up in different .sst files.",
                  new BooleanParameter(&_partitionFilesForMdiIndexCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(Enabling this option will make RocksDB's
  compaction write the persistent index data for different mdi
  indexes (also indexes from different collections/shards) into different
  .sst files. Otherwise the persistent index data from different
  collections/shards/indexes can be mixed and written into the same .sst files.

  Enabling this option usually has the benefit of making the RocksDB
  compaction more efficient when a lot of different collections/shards/indexes
  are written to in parallel.
  The disavantage of enabling this option is that there can be more .sst
  files than when the option is turned off, and the disk space used by
  these .sst files can be higher than if there are fewer .sst files (this
  is because there is some per-.sst file overhead).
  In particular on deployments with many collections/shards/indexes
  this can lead to a very high number of .sst files, with the potential
  of outgrowing the maximum number of file descriptors the ArangoDB process
  can open. Thus the option should only be enabled on deployments with a
  limited number of edge collections/shards/indexes.)");

  options
      ->addOption("--rocksdb.partition-files-for-vector-index",
                  "If enabled, the index data for different vector "
                  "indexes will end up in different .sst files.",
                  new BooleanParameter(&_partitionFilesForVectorIndexCf),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31204)
      .setLongDescription(R"(Enabling this option makes RocksDB's
compaction write the index data for different vector
indexes (also indexes from different collections/shards) into different
.sst files. Otherwise, the index data from different
collections/shards/indexes can be mixed and written into the same .sst files.

Enabling this option usually has the benefit of making the RocksDB
compaction more efficient when a lot of different collections/shards/indexes
are written to in parallel.
The disadvantage of enabling this option is that there can be more .sst
files than when the option is disabled, and the disk space used by
these .sst files can be higher than if there are fewer .sst files
because there is some overhead per .sst file.
For deployments with many collections/shards/indexes in particular,
this can lead to a very high number of .sst files, with the potential
of outgrowing the maximum number of file descriptors the ArangoDB process
can open. The option should thus only be enabled for deployments with a
limited number of edge collections/shards/indexes.)");

  options
      ->addOption(
          "--rocksdb.use-io_uring",
          "Check for existence of io_uring at startup and use it if available. "
          "Should be set to false only to opt out of using io_uring.",
          new BooleanParameter(&ioUringEnabled),
          arangodb::options::makeFlags(arangodb::options::Flags::Uncommon,
                                       arangodb::options::Flags::OnAgent,
                                       arangodb::options::Flags::OnDBServer,
                                       arangodb::options::Flags::OnSingle))
      .setIntroducedIn(3'12'00);

  //////////////////////////////////////////////////////////////////////////////
  /// add column family-specific options now
  //////////////////////////////////////////////////////////////////////////////
  static constexpr std::initializer_list<RocksDBColumnFamilyManager::Family>
      families{RocksDBColumnFamilyManager::Family::Definitions,
               RocksDBColumnFamilyManager::Family::Documents,
               RocksDBColumnFamilyManager::Family::PrimaryIndex,
               RocksDBColumnFamilyManager::Family::EdgeIndex,
               RocksDBColumnFamilyManager::Family::VPackIndex,
               RocksDBColumnFamilyManager::Family::GeoIndex,
               RocksDBColumnFamilyManager::Family::FulltextIndex,
               RocksDBColumnFamilyManager::Family::ReplicatedLogs,
               RocksDBColumnFamilyManager::Family::MdiIndex,
               RocksDBColumnFamilyManager::Family::MdiVPackIndex,
               RocksDBColumnFamilyManager::Family::VectorIndex};

  auto addMaxWriteBufferNumberCf =
      [this, &options](RocksDBColumnFamilyManager::Family family) {
        std::string name = RocksDBColumnFamilyManager::name(
            family, RocksDBColumnFamilyManager::NameMode::External);
        std::size_t index = static_cast<
            std::underlying_type<RocksDBColumnFamilyManager::Family>::type>(
            family);
        auto introducedIn = 30800;
        if (family == RocksDBColumnFamilyManager::Family::MdiVPackIndex ||
            family == RocksDBColumnFamilyManager::Family::MdiIndex) {
          introducedIn = 31200;
        }
        if (family == RocksDBColumnFamilyManager::Family::VectorIndex) {
          introducedIn = 31204;
        }

        options
            ->addOption("--rocksdb.max-write-buffer-number-" + name,
                        "If non-zero, overrides the value of "
                        "`--rocksdb.max-write-buffer-number` for the " +
                            name + " column family",
                        new UInt64Parameter(&_maxWriteBufferNumberCf[index]),
                        arangodb::options::makeDefaultFlags(
                            arangodb::options::Flags::Uncommon))
            .setIntroducedIn(introducedIn);
      };
  for (auto family : families) {
    addMaxWriteBufferNumberCf(family);
  }
}

void RocksDBOptionFeature::validateOptions(
    std::shared_ptr<ProgramOptions> options) {
  if (_writeBufferSize > 0 && _writeBufferSize < 1024 * 1024) {
    LOG_TOPIC("4ce44", FATAL, arangodb::Logger::STARTUP)
        << "invalid value for '--rocksdb.write-buffer-size'";
    FATAL_ERROR_EXIT();
  }
  if (_totalWriteBufferSize > 0 && _totalWriteBufferSize < 64 * 1024 * 1024) {
    LOG_TOPIC("4ab88", FATAL, arangodb::Logger::STARTUP)
        << "invalid value for '--rocksdb.total-write-buffer-size'";
    FATAL_ERROR_EXIT();
  }
  if (_maxBackgroundJobs != -1 && _maxBackgroundJobs < 1) {
    LOG_TOPIC("cfc5a", FATAL, arangodb::Logger::STARTUP)
        << "invalid value for '--rocksdb.max-background-jobs'";
    FATAL_ERROR_EXIT();
  }

  _minWriteBufferNumberToMergeTouched = options->processingResult().touched(
      "--rocksdb.min-write-buffer-number-to-merge");

  // limit memory usage of agent instances, if not otherwise configured
  if (server().hasFeature<AgencyFeature>()) {
    AgencyFeature& feature = server().getFeature<AgencyFeature>();
    if (feature.activated()) {
      // if we are an agency instance...
      if (!options->processingResult().touched("--rocksdb.block-cache-size")) {
        // restrict block cache size to 1 GB if not set explicitly
        _blockCacheSize =
            std::min<uint64_t>(_blockCacheSize, uint64_t(1) << 30);
      }
      if (!options->processingResult().touched(
              "--rocksdb.total-write-buffer-size")) {
        // restrict total write buffer size to 512 MB if not set explicitly
        _totalWriteBufferSize =
            std::min<uint64_t>(_totalWriteBufferSize, uint64_t(512) << 20);
      }
    }
  }

  if (_blockCacheType == ::kBlockCacheTypeLRU &&
      options->processingResult().touched(
          "--rocksdb.block-cache-estimated-entry-charge")) {
    LOG_TOPIC("a527b", WARN, arangodb::Logger::ENGINES)
        << "Setting value of '--rocksdb.block-cache-estimated-entry-charge' "
           "has no effect when using LRU block cache";
  }

  if (_enforceBlockCacheSizeLimit && !options->processingResult().touched(
                                         "--rocksdb.block-cache-shard-bits")) {
    // if block cache size limit is enforced, and the number of shard bits for
    // the block cache hasn't been set, we set it dynamically:
    // we would like that each block cache shard can hold data blocks of
    // at least a common size. Rationale: data blocks can be quite large. if
    // they don't fit into the block cache upon reading, the block cache will
    // return Status::Incomplete() or Status::MemoryLimit() when the block
    // cache's strict capacity limit is set. then we cannot read data anymore.
    // we are limiting the maximum number of shard bits to 10 here, which is
    // 1024 shards. that should be enough shards even for very big caches.
    // note that RocksDB also has an internal upper bound for the number of
    // shards bits, which is 20.
    _blockCacheShardBits = std::clamp(
        int64_t(std::floor(
            std::log2(static_cast<double>(_blockCacheSize) / ::minShardSize))),
        int64_t(1), int64_t(10));

    // TODO: hyper clock cache probably doesn't need as many shards. check this.
  }

#ifndef ARANGODB_HAVE_JEMALLOC
  // on some platforms, jemalloc is not available, because it is not compiled
  // in by default. to make the startup of the server not fail in such
  // environment, turn off the option automatically
  if (_useJemallocAllocator) {
    _useJemallocAllocator = false;
    LOG_TOPIC("b3164", INFO, Logger::STARTUP)
        << "disabling jemalloc allocator for RocksDB - jemalloc not compiled";
  }
#endif

  if (!_enableBlobFiles) {
    // turn off blob garbage collection to avoid potential side effects
    // for performance
    _enableBlobGarbageCollection = false;
  }
}

void RocksDBOptionFeature::prepare() {
  if (_enableBlobFiles) {
    LOG_TOPIC("5e48f", WARN, Logger::ENGINES)
        << "using blob files is experimental and not supported for production "
           "usage";
  }

  if (_compactionStyle != ::kCompactionStyleLevel) {
    LOG_TOPIC("6db54", WARN, Logger::ENGINES)
        << "using compaction style '" << _compactionStyle
        << "' is experimental and not supported for production usage";
  }

  if (_blockCacheType == ::kBlockCacheTypeHyperClock) {
    LOG_TOPIC("26f64", WARN, Logger::ENGINES)
        << "using block cache type 'hyper-clock' is experimental and not "
           "supported for production usage";
  }

  if (_enforceBlockCacheSizeLimit && _blockCacheSize > 0) {
    uint64_t shardSize =
        _blockCacheSize / (uint64_t(1) << _blockCacheShardBits);
    // if we can't store a data block of the mininmum size in the block cache,
    // we may run into problems when trying to put a large data block into the
    // cache. in this case the block cache may return a Status::Incomplete()
    // or Status::MemoryLimit() error and fail the entire read.
    // warn the user about it!
    if (shardSize < ::minShardSize) {
      LOG_TOPIC("31d7c", WARN, Logger::ENGINES)
          << "size of RocksDB block cache shards seems to be too low. "
          << "block cache size: " << _blockCacheSize
          << ", shard bits: " << _blockCacheShardBits
          << ", shard size: " << shardSize
          << ". it is probably useful to set "
             "`--rocksdb.enforce-block-cache-size-limit` to false "
          << "to avoid incomplete cache reads.";
    }
  }
}

void RocksDBOptionFeature::start() {
  uint32_t max = _maxBackgroundJobs / 2;
  uint32_t clamped = std::max(
      std::min(static_cast<uint32_t>(NumberOfCores::getValue()), max), 1U);
  // lets test this out
  if (_numThreadsHigh == 0) {
    _numThreadsHigh = clamped;
  }
  if (_numThreadsLow == 0) {
    _numThreadsLow = clamped;
  }

  if (_maxSubcompactions > _numThreadsLow) {
    if (server().options()->processingResult().touched(
            "--rocksdb.max-subcompactions")) {
      LOG_TOPIC("e7c85", WARN, Logger::ENGINES)
          << "overriding value for option `--rocksdb.max-subcompactions` to "
          << _numThreadsLow
          << " because the specified value is greater than the number of "
             "threads for low priority operations";
    }
    _maxSubcompactions = _numThreadsLow;
  }

  LOG_TOPIC("f66e4", TRACE, Logger::ENGINES)
      << "using RocksDB options:"
      << " wal_dir: '" << _walDirectory << "'"
      << ", compression type: " << _compressionType
      << ", write_buffer_size: " << _writeBufferSize
      << ", total_write_buffer_size: " << _totalWriteBufferSize
      << ", max_write_buffer_number: " << _maxWriteBufferNumber
      << ", max_write_buffer_size_to_maintain: "
      << _maxWriteBufferSizeToMaintain
      << ", max_total_wal_size: " << _maxTotalWalSize
      << ", delayed_write_rate: " << _delayedWriteRate
      << ", min_write_buffer_number_to_merge: " << _minWriteBufferNumberToMerge
      << ", num_levels: " << _numLevels
      << ", num_uncompressed_levels: " << _numUncompressedLevels
      << ", max_bytes_for_level_base: " << _maxBytesForLevelBase
      << ", max_bytes_for_level_multiplier: " << _maxBytesForLevelMultiplier
      << ", max_background_jobs: " << _maxBackgroundJobs
      << ", max_sub_compactions: " << _maxSubcompactions
      << ", target_file_size_base: " << _targetFileSizeBase
      << ", target_file_size_multiplier: " << _targetFileSizeMultiplier
      << ", num_threads_high: " << _numThreadsHigh
      << ", num_threads_low: " << _numThreadsLow
      << ", block_cache_type: " << _blockCacheType
      << ", use_jemalloc_allocator: " << _useJemallocAllocator
      << ", block_cache_size: " << _blockCacheSize
      << ", block_cache_shard_bits: " << _blockCacheShardBits
      << ", block_cache_estimated_entry_charge: "
      << _blockCacheEstimatedEntryCharge
      << ", block_cache_strict_capacity_limit: " << std::boolalpha
      << _enforceBlockCacheSizeLimit
      << ", cache_index_and_filter_blocks: " << std::boolalpha
      << _cacheIndexAndFilterBlocks
      << ", cache_index_and_filter_blocks_with_high_priority: "
      << std::boolalpha << _cacheIndexAndFilterBlocksWithHighPriority
      << ", pin_l0_filter_and_index_blocks_in_cache: " << std::boolalpha
      << _pinl0FilterAndIndexBlocksInCache
      << ", pin_top_level_index_and_filter: " << std::boolalpha
      << _pinTopLevelIndexAndFilter << ", table_block_size: " << _tableBlockSize
      << ", recycle_log_file_num: " << _recycleLogFileNum
      << ", compaction_read_ahead_size: " << _compactionReadaheadSize
      << ", level0_compaction_trigger: " << _level0CompactionTrigger
      << ", level0_slowdown_trigger: " << _level0SlowdownTrigger
      << ", periodic_compaction_ttl: " << _periodicCompactionTtl
      << ", checksum: " << _checksumType
      << ", format_version: " << _formatVersion
      << ", bloom_bits_per_key: " << _bloomBitsPerKey
      << ", enable_blob_files: " << std::boolalpha << _enableBlobFiles
      << ", enable_blob_cache: " << std::boolalpha << _enableBlobCache
      << ", min_blob_size: " << _minBlobSize
      << ", blob_file_size: " << _blobFileSize
      << ", blob_file_starting_level: " << _blobFileStartingLevel
      << ", blob_compression type: " << _blobCompressionType
      << ", enable_blob_garbage_collection: " << std::boolalpha
      << _enableBlobGarbageCollection
      << ", blob_garbage_collection_age_cutoff: "
      << _blobGarbageCollectionAgeCutoff
      << ", blob_garbage_collection_force_threshold: "
      << _blobGarbageCollectionForceThreshold
      << ", prepopulate_blob_cache: " << std::boolalpha << _prepopulateBlobCache
      << ", enable_index_compression: " << std::boolalpha
      << _enableIndexCompression
      << ", prepopulate_block_cache: " << std::boolalpha
      << _prepopulateBlockCache
      << ", reserve_table_builder_memory: " << std::boolalpha
      << _reserveTableBuilderMemory
      << ", reserve_table_reader_memory: " << std::boolalpha
      << _reserveTableReaderMemory
      << ", enable_pipelined_write: " << std::boolalpha << _enablePipelinedWrite
      << ", optimize_filters_for_hits: " << std::boolalpha
      << _optimizeFiltersForHits << ", use_direct_reads: " << std::boolalpha
      << _useDirectReads
      << ", use_direct_io_for_flush_and_compaction: " << std::boolalpha
      << _useDirectIoForFlushAndCompaction << ", use_fsync: " << std::boolalpha
      << _useFSync << ", allow_fallocate: " << std::boolalpha << _allowFAllocate
      << ", max_open_files limit: " << std::boolalpha
      << _limitOpenFilesAtStartup
      << ", dynamic_level_bytes: " << _dynamicLevelBytes;
}

rocksdb::TransactionDBOptions RocksDBOptionFeature::getTransactionDBOptions()
    const {
  rocksdb::TransactionDBOptions result;
  // number of locks per column_family
  result.num_stripes =
      std::max(size_t(1), static_cast<size_t>(_transactionLockStripes));
  result.transaction_lock_timeout = _transactionLockTimeout;
  return result;
}

rocksdb::Options RocksDBOptionFeature::doGetOptions() const {
  rocksdb::Options result;
  result.allow_fallocate = _allowFAllocate;
  result.enable_pipelined_write = _enablePipelinedWrite;
  result.write_buffer_size = static_cast<size_t>(_writeBufferSize);
  result.max_write_buffer_number = static_cast<int>(_maxWriteBufferNumber);
  // The following setting deserves an explanation: We found that if we leave
  // the default for max_write_buffer_number_to_maintain at 0, then setting
  // max_write_buffer_size_to_maintain to 0 has not the desired effect, rather
  // TransactionDB::PrepareWrap then sets the latter to -1 which in turn is
  // later corrected to max_write_buffer_number * write_buffer_size.
  // Therefore, we set the deprecated option max_write_buffer_number_to_maintain
  // to 1, so that we can then configure max_write_buffer_size_to_maintain
  // correctly. Set to -1, 0 or a concrete number as needed. The default of
  // 0 should be good, since we do not use OptimisticTransactionDBs anyway.
  result.max_write_buffer_number_to_maintain = 1;
  result.max_write_buffer_size_to_maintain = _maxWriteBufferSizeToMaintain;
  result.delayed_write_rate = _delayedWriteRate;
  result.min_write_buffer_number_to_merge =
      static_cast<int>(_minWriteBufferNumberToMerge);
  result.num_levels = static_cast<int>(_numLevels);
  result.level_compaction_dynamic_level_bytes = _dynamicLevelBytes;
  result.max_bytes_for_level_base = _maxBytesForLevelBase;
  result.max_bytes_for_level_multiplier =
      static_cast<int>(_maxBytesForLevelMultiplier);
  result.optimize_filters_for_hits = _optimizeFiltersForHits;
  result.use_direct_reads = _useDirectReads;
  result.use_direct_io_for_flush_and_compaction =
      _useDirectIoForFlushAndCompaction;

  result.target_file_size_base = _targetFileSizeBase;
  result.target_file_size_multiplier =
      static_cast<int>(_targetFileSizeMultiplier);
  // during startup, limit the total WAL size to a small value so we do not see
  // large WAL files created at startup.
  // Instead, we will start with a small value here and up it later in the
  // startup process
  result.max_total_wal_size = 4 * 1024 * 1024;

  result.wal_dir = _walDirectory;

  if (_skipCorrupted) {
    result.wal_recovery_mode =
        rocksdb::WALRecoveryMode::kSkipAnyCorruptedRecords;
  } else {
    result.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;
  }

  result.max_background_jobs = static_cast<int>(_maxBackgroundJobs);
  result.max_subcompactions = _maxSubcompactions;
  result.use_fsync = _useFSync;

  rocksdb::CompressionType compressionType =
      ::compressionTypeFromString(_compressionType);

  // only compress levels >= 2
  result.compression_per_level.resize(result.num_levels);
  for (int level = 0; level < result.num_levels; ++level) {
    result.compression_per_level[level] =
        ((static_cast<uint64_t>(level) >= _numUncompressedLevels)
             ? compressionType
             : rocksdb::kNoCompression);
  }

  result.compaction_style = ::compactionStyleFromString(_compactionStyle);
  result.compaction_pri = rocksdb::kMinOverlappingRatio;

  // Number of files to trigger level-0 compaction. A value <0 means that
  // level-0 compaction will not be triggered by number of files at all.
  // Default: 4
  result.level0_file_num_compaction_trigger =
      static_cast<int>(_level0CompactionTrigger);

  // Soft limit on number of level-0 files. We start slowing down writes at this
  // point. A value <0 means that no writing slow down will be triggered by
  // number of files in level-0.
  result.level0_slowdown_writes_trigger =
      static_cast<int>(_level0SlowdownTrigger);

  // Maximum number of level-0 files.  We stop writes at this point.
  result.level0_stop_writes_trigger = static_cast<int>(_level0StopTrigger);

  // Soft limit on pending compaction bytes. We start slowing down writes
  // at this point.
  result.soft_pending_compaction_bytes_limit =
      _pendingCompactionBytesSlowdownTrigger;

  // Maximum number of pending compaction bytes. We stop writes at this point.
  result.hard_pending_compaction_bytes_limit =
      _pendingCompactionBytesStopTrigger;

  // table cache is only used when max_open_files != -1
  result.table_cache_numshardbits = 8;

  result.recycle_log_file_num = _recycleLogFileNum;
  result.compaction_readahead_size =
      static_cast<size_t>(_compactionReadaheadSize);

  // intentionally set the RocksDB logger to ERROR because it will
  // log lots of things otherwise
  if (!_useFileLogging) {
    // if we don't use file logging but log into ArangoDB's logfile,
    // we only want real errors
    result.info_log_level = rocksdb::InfoLogLevel::ERROR_LEVEL;
  }

  if (_enableStatistics) {
    result.statistics = rocksdb::CreateDBStatistics();
    // result.stats_dump_period_sec = 1;
  }

  result.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(getTableOptions()));

  result.create_if_missing = true;
  result.create_missing_column_families = true;

  if (_limitOpenFilesAtStartup) {
    result.max_open_files = 16;
    result.skip_stats_update_on_db_open = true;
    result.avoid_flush_during_recovery = true;
  } else {
    result.max_open_files = -1;
  }

  if (_totalWriteBufferSize > 0) {
    result.db_write_buffer_size = _totalWriteBufferSize;
  }

  // we manage WAL file deletion ourselves, don't let RocksDB garbage-collect
  // obsolete files.
  result.WAL_ttl_seconds =
      933120000;  // ~30 years (60 * 60 * 24 * 30 * 12 * 30)
  result.WAL_size_limit_MB = 0;
  result.memtable_prefix_bloom_size_ratio = 0.2;  // TODO: pick better value?
  // TODO: enable memtable_insert_with_hint_prefix_extractor?
  result.bloom_locality = 1;

  if (!server().options()->processingResult().touched(
          "rocksdb.max-write-buffer-number")) {
    // TODO It is unclear if this value makes sense as a default, but we aren't
    // changing it yet, in order to maintain backwards compatibility.

    // user hasn't explicitly set the number of write buffers, so we use a
    // default value based on the number of column families this is
    // cfFamilies.size() + 2 ... but _option needs to be set before
    //  building cfFamilies
    // Update max_write_buffer_number above if you change number of families
    // used
    result.max_write_buffer_number = 8 + 2;
  } else if (result.max_write_buffer_number < 4) {
    // user set the value explicitly, and it is lower than recommended
    result.max_write_buffer_number = 4;
    LOG_TOPIC("d5c49", WARN, Logger::ENGINES)
        << "overriding value for option `--rocksdb.max-write-buffer-number` "
           "to 4 because it is lower than recommended";
  }

  return result;
}

rocksdb::BlockBasedTableOptions RocksDBOptionFeature::doGetTableOptions()
    const {
  rocksdb::BlockBasedTableOptions result;

  if (_blockCacheSize > 0) {
    std::shared_ptr<rocksdb::MemoryAllocator> allocator;

#ifdef ARANGODB_HAVE_JEMALLOC
    if (_useJemallocAllocator) {
      rocksdb::JemallocAllocatorOptions jopts;
      rocksdb::Status s =
          rocksdb::NewJemallocNodumpAllocator(jopts, &allocator);
      if (!s.ok()) {
        LOG_TOPIC("004e6", FATAL, Logger::STARTUP)
            << "unable to use jemalloc allocator for RocksDB: " << s.ToString();
        FATAL_ERROR_EXIT();
      }
    }
#endif

    if (_blockCacheType == ::kBlockCacheTypeLRU) {
      rocksdb::LRUCacheOptions opts;

      opts.capacity = _blockCacheSize;
      opts.num_shard_bits = static_cast<int>(_blockCacheShardBits);
      opts.strict_capacity_limit = _enforceBlockCacheSizeLimit;
      opts.memory_allocator = allocator;

      result.block_cache = rocksdb::NewLRUCache(opts);
    } else if (_blockCacheType == ::kBlockCacheTypeHyperClock) {
      rocksdb::HyperClockCacheOptions opts(
          _blockCacheSize, _blockCacheEstimatedEntryCharge,
          static_cast<int>(_blockCacheShardBits), _enforceBlockCacheSizeLimit,
          allocator);

      result.block_cache = opts.MakeSharedCache();
    } else {
      TRI_ASSERT(false);
    }

  } else {
    result.no_block_cache = true;
  }

  result.cache_index_and_filter_blocks = _cacheIndexAndFilterBlocks;
  result.cache_index_and_filter_blocks_with_high_priority =
      _cacheIndexAndFilterBlocksWithHighPriority;
  result.pin_l0_filter_and_index_blocks_in_cache =
      _pinl0FilterAndIndexBlocksInCache;
  result.pin_top_level_index_and_filter = _pinTopLevelIndexAndFilter;

  result.block_size = _tableBlockSize;
  result.filter_policy.reset(
      rocksdb::NewBloomFilterPolicy(_bloomBitsPerKey, true));
  result.enable_index_compression = _enableIndexCompression;
  result.format_version = _formatVersion;
  result.optimize_filters_for_memory = _optimizeFiltersForMemory;
  result.prepopulate_block_cache =
      _prepopulateBlockCache
          ? rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushOnly
          : rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kDisable;
  result.cache_usage_options.options_overrides.insert(
      {rocksdb::CacheEntryRole::kFilterConstruction,
       {/*.charged = */ _reserveTableBuilderMemory
            ? rocksdb::CacheEntryRoleOptions::Decision::kEnabled
            : rocksdb::CacheEntryRoleOptions::Decision::kDisabled}});
  result.cache_usage_options.options_overrides.insert(
      {rocksdb::CacheEntryRole::kBlockBasedTableReader,
       {/*.charged = */ _reserveTableReaderMemory
            ? rocksdb::CacheEntryRoleOptions::Decision::kEnabled
            : rocksdb::CacheEntryRoleOptions::Decision::kDisabled}});
  result.cache_usage_options.options_overrides.insert(
      {rocksdb::CacheEntryRole::kFileMetadata,
       {/*.charged = */ _reserveFileMetadataMemory
            ? rocksdb::CacheEntryRoleOptions::Decision::kEnabled
            : rocksdb::CacheEntryRoleOptions::Decision::kDisabled}});

  result.block_align = _blockAlignDataBlocks;

  if (_checksumType == ::kChecksumTypeCRC32C) {
    result.checksum = rocksdb::ChecksumType::kCRC32c;
  } else if (_checksumType == ::kChecksumTypeXXHash) {
    result.checksum = rocksdb::ChecksumType::kxxHash;
  } else if (_checksumType == ::kChecksumTypeXXHash64) {
    result.checksum = rocksdb::ChecksumType::kxxHash64;
  } else if (_checksumType == ::kChecksumTypeXXH3) {
    result.checksum = rocksdb::ChecksumType::kXXH3;
  } else {
    TRI_ASSERT(false);
    LOG_TOPIC("8d602", WARN, arangodb::Logger::STARTUP)
        << "unexpected value for '--rocksdb.checksum-type'";
  }

  return result;
}

rocksdb::ColumnFamilyOptions RocksDBOptionFeature::getColumnFamilyOptions(
    RocksDBColumnFamilyManager::Family family) const {
  rocksdb::ColumnFamilyOptions result =
      RocksDBOptionsProvider::getColumnFamilyOptions(family);

  if (family == RocksDBColumnFamilyManager::Family::Documents) {
    result.enable_blob_files = _enableBlobFiles;
    result.min_blob_size = _minBlobSize;
    result.blob_file_size = _blobFileSize;
    result.blob_compression_type =
        ::compressionTypeFromString(_blobCompressionType);
    result.enable_blob_garbage_collection = _enableBlobGarbageCollection;
    result.blob_garbage_collection_age_cutoff = _blobGarbageCollectionAgeCutoff;
    result.blob_garbage_collection_force_threshold =
        _blobGarbageCollectionForceThreshold;
    result.blob_file_starting_level = _blobFileStartingLevel;
    result.prepopulate_blob_cache =
        _prepopulateBlobCache ? rocksdb::PrepopulateBlobCache::kFlushOnly
                              : rocksdb::PrepopulateBlobCache::kDisable;
    if (_enableBlobCache) {
      // use whatever block cache we use for blobs as well
      result.blob_cache = getTableOptions().block_cache;
    }
    if (_partitionFilesForDocumentsCf) {
      // partition .sst files by object id prefix
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
  }

  if (family == RocksDBColumnFamilyManager::Family::PrimaryIndex) {
    // partition .sst files by object id prefix
    if (_partitionFilesForPrimaryIndexCf) {
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
    // keep immutable mem tables around in memory for conflict checking
    result.max_write_buffer_size_to_maintain = 64 << 20;
  }

  if (family == RocksDBColumnFamilyManager::Family::EdgeIndex) {
    // partition .sst files by object id prefix
    if (_partitionFilesForEdgeIndexCf) {
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
  }

  if (family == RocksDBColumnFamilyManager::Family::VPackIndex) {
    // partition .sst files by object id prefix
    if (_partitionFilesForVPackIndexCf) {
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
  }

  if (family == RocksDBColumnFamilyManager::Family::MdiIndex ||
      family == RocksDBColumnFamilyManager::Family::MdiVPackIndex) {
    // partition .sst files by object id prefix
    if (_partitionFilesForMdiIndexCf) {
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
  }
  if (family == RocksDBColumnFamilyManager::Family::VectorIndex) {
    // partition .sst files by object id prefix
    if (_partitionFilesForVectorIndexCf) {
      result.sst_partitioner_factory =
          rocksdb::NewSstPartitionerFixedPrefixFactory(sizeof(uint64_t));
    }
  }

  // override
  std::size_t index = static_cast<
      std::underlying_type<RocksDBColumnFamilyManager::Family>::type>(family);
  TRI_ASSERT(index < _maxWriteBufferNumberCf.size());
  if (_maxWriteBufferNumberCf[index] > 0) {
    result.max_write_buffer_number =
        static_cast<int>(_maxWriteBufferNumberCf[index]);
  }
  if (!_minWriteBufferNumberToMergeTouched) {
    result.min_write_buffer_number_to_merge =
        static_cast<int>(defaultMinWriteBufferNumberToMerge(
            _totalWriteBufferSize, _writeBufferSize,
            result.max_write_buffer_number));
  }

  return result;
}
