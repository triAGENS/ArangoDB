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

#pragma once

#include <memory>

#include "RocksDBEngine/RocksDBOptionsProvider.h"

namespace arangodb::sepp {

struct RocksDBOptions : arangodb::RocksDBOptionsProvider {
  RocksDBOptions();

  rocksdb::TransactionDBOptions getTransactionDBOptions() const override;
  rocksdb::Options getOptions() const override;
  rocksdb::BlockBasedTableOptions getTableOptions() const override;

  uint64_t maxTotalWalSize() const noexcept override;
  uint32_t numThreadsHigh() const noexcept override;
  uint32_t numThreadsLow() const noexcept override;

  struct GeneralOptions {
    uint32_t numThreadsLow;
    uint32_t numThreadsHigh;

    uint64_t maxTotalWalSize;

    bool allowFAllocate;
    bool enablePipelinedWrite;
    uint64_t writeBufferSize;
    uint32_t maxWriteBufferNumber;
    uint32_t maxWriteBufferNumberToMaintain = 1;
    int64_t maxWriteBufferSizeToMaintain;
    uint64_t delayedWriteRate;
    uint64_t minWriteBufferNumberToMerge;
    uint64_t numLevels;
    bool levelCompactionDynamicLevelBytes;
    uint64_t maxBytesForLevelBase;
    double maxBytesForLevelMultiplier;
    bool optimizeFiltersForHits;
    bool useDirectReads;
    bool useDirectIoForFlushAndCompaction;

    uint64_t targetFileSizeBase;
    uint64_t targetFileSizeMultiplier;

    int32_t maxBackgroundJobs;
    uint32_t maxSubcompactions;
    bool useFSync;

    uint32_t numUncompressedLevels;

    // TODO - compression algo

    // Number of files to trigger level-0 compaction. A value <0 means that
    // level-0 compaction will not be triggered by number of files at all.
    // Default: 4
    int64_t level0FileNumCompactionTrigger;

    // Soft limit on number of level-0 files. We start slowing down writes at
    // this point. A value <0 means that no writing slow down will be triggered
    // by number of files in level-0.
    int64_t level0SlowdownWritesTrigger;

    // Maximum number of level-0 files.  We stop writes at this point.
    int64_t level0StopWritesTrigger;

    // Soft limit on pending compaction bytes. We start slowing down writes
    // at this point.
    uint64_t pendingCompactionBytesSlowdownTrigger;

    // Maximum number of pending compaction bytes. We stop writes at this point.
    uint64_t pendingCompactionBytesStopTrigger;

    size_t recycleLogFileNum;
    uint64_t compactionReadaheadSize;

    bool enableStatistics;

    uint64_t totalWriteBufferSize;

    double memtablePrefixBloomSizeRatio = 0.2;
    // TODO: memtable_insert_with_hint_prefix_extractor?
    uint32_t bloomLocality = 1;

    template<class Inspector>
    inline friend auto inspect(Inspector& f, GeneralOptions& o) {
      return f.object(o).fields(
          f.field("numThreadsLow", o.numThreadsLow),
          f.field("numThreadsHigh", o.numThreadsHigh),

          f.field("maxTotalWalSize", o.maxTotalWalSize),
          f.field("allowFAllocate", o.allowFAllocate),
          f.field("enablePipelinedWrite", o.enablePipelinedWrite),
          f.field("writeBufferSize", o.writeBufferSize),
          f.field("maxWriteBufferNumber", o.maxWriteBufferNumber),
          f.field("maxWriteBufferNumberToMaintain",
                  o.maxWriteBufferNumberToMaintain),
          f.field("maxWriteBufferSizeToMaintain",
                  o.maxWriteBufferSizeToMaintain),
          f.field("delayedWriteRate", o.delayedWriteRate),
          f.field("minWriteBufferNumberToMerge", o.minWriteBufferNumberToMerge),
          f.field("numLevels", o.numLevels),
          f.field("levelCompactionDynamicLevelBytes",
                  o.levelCompactionDynamicLevelBytes),
          f.field("maxBytesForLevelBase", o.maxBytesForLevelBase),
          f.field("maxBytesForLevelMultiplier", o.maxBytesForLevelMultiplier),
          f.field("optimizeFiltersForHits", o.optimizeFiltersForHits),
          f.field("useDirectReads", o.useDirectReads),
          f.field("useDirectIoForFlushAndCompaction",
                  o.useDirectIoForFlushAndCompaction),
          f.field("targetFileSizeBase", o.targetFileSizeBase),
          f.field("targetFileSizeMultiplier", o.targetFileSizeMultiplier),
          f.field("maxBackgroundJobs", o.maxBackgroundJobs),
          f.field("maxSubcompactions", o.maxSubcompactions),
          f.field("useFSync", o.useFSync),
          f.field("numUncompressedLevels", o.numUncompressedLevels),
          f.field("level0FileNumCompactionTrigger",
                  o.level0FileNumCompactionTrigger),
          f.field("level0SlowdownWritesTrigger", o.level0SlowdownWritesTrigger),
          f.field("level0StopWritesTrigger", o.level0StopWritesTrigger),
          f.field("pendingCompactionBytesSlowdownTrigger",
                  o.pendingCompactionBytesSlowdownTrigger),
          f.field("pendingCompactionBytesStopTrigger",
                  o.pendingCompactionBytesStopTrigger),
          f.field("recycleLogFileNum", o.recycleLogFileNum),
          f.field("compactionReadaheadSize", o.compactionReadaheadSize),
          f.field("enableStatistics", o.enableStatistics),
          f.field("totalWriteBufferSize", o.totalWriteBufferSize),
          f.field("memtablePrefixBloomSizeRatio",
                  o.memtablePrefixBloomSizeRatio),
          f.field("bloomLocality", o.bloomLocality));
    }

    // The following is a complete list of RocksDB options we currently do not
    // touch.

    // DBOptions
    /*
        bool paranoid_checks = true;
        bool flush_verify_memtable_count = true;
        bool track_and_verify_wals_in_manifest = false;
        std::shared_ptr<RateLimiter> rate_limiter = nullptr;
        std::shared_ptr<SstFileManager> sst_file_manager = nullptr;

        int max_file_opening_threads = 16;
        uint64_t delete_obsolete_files_period_micros = 6ULL * 60 * 60 * 1000000;
        int base_background_compactions = -1;
        int max_background_compactions = -1;
        int max_background_flushes = -1;
        size_t max_log_file_size = 0;
        size_t log_file_time_to_roll = 0;
        size_t keep_log_file_num = 1000;
        uint64_t max_manifest_file_size = 1024 * 1024 * 1024;
        int table_cache_numshardbits = 6;
        size_t manifest_preallocation_size = 4 * 1024 * 1024;
        bool allow_mmap_reads = false;
        bool allow_mmap_writes = false;
        bool is_fd_close_on_exec = true;
        bool skip_log_error_on_recovery = false;
        unsigned int stats_dump_period_sec = 600;
        unsigned int stats_persist_period_sec = 600;
        bool persist_stats_to_disk = false;
        size_t stats_history_buffer_size = 1024 * 1024;
        bool advise_random_on_open = true;
        double experimental_mempurge_threshold = 0.0;
        std::shared_ptr<WriteBufferManager> write_buffer_manager = nullptr;
        AccessHint access_hint_on_compaction_start = NORMAL;
        bool new_table_reader_for_compaction_inputs = false;
        size_t random_access_max_buffer_size = 1024 * 1024;
        size_t writable_file_max_buffer_size = 1024 * 1024;
        bool use_adaptive_mutex = false;

        uint64_t bytes_per_sync = 0;
        uint64_t wal_bytes_per_sync = 0;
        bool strict_bytes_per_sync = false;
        std::vector<std::shared_ptr<EventListener>> listeners;
        bool enable_thread_tracking = false;
        bool unordered_write = false;
        bool allow_concurrent_memtable_write = true;
        bool enable_write_thread_adaptive_yield = true;
        uint64_t max_write_batch_group_size_bytes = 1 << 20;
        uint64_t write_thread_max_yield_usec = 100;
        uint64_t write_thread_slow_yield_usec = 3;
        bool skip_stats_update_on_db_open = false;
        bool skip_checking_sst_file_sizes_on_db_open = false;
        bool allow_2pc = false;
        std::shared_ptr<Cache> row_cache = nullptr;
        WalFilter* wal_filter = nullptr;
        bool fail_if_options_file_error = false;
        bool dump_malloc_stats = false;
        bool avoid_flush_during_recovery = false;
        bool avoid_flush_during_shutdown = false;
        bool allow_ingest_behind = false;
        bool preserve_deletes = false;
        bool two_write_queues = false;
        bool manual_wal_flush = false;
        bool atomic_flush = false;
        bool avoid_unnecessary_blocking_io = false;
        bool write_dbid_to_manifest = false;
        size_t log_readahead_size = 0;
        std::shared_ptr<FileChecksumGenFactory> file_checksum_gen_factory;
        bool best_efforts_recovery = false;
        int max_bgerror_resume_count = INT_MAX;
        uint64_t bgerror_resume_retry_interval = 1000000;

        bool allow_data_in_errors = false;
        std::string db_host_id = kHostnameForDbHostId;
        FileTypeSet checksum_handoff_file_types;
        std::shared_ptr<CompactionService> compaction_service = nullptr;

        CacheTier lowest_used_cache_tier = CacheTier::kNonVolatileBlockTier;
    */

    // ColumnFamily Options
    /*
      std::shared_ptr<MergeOperator> merge_operator = nullptr;
      const CompactionFilter* compaction_filter = nullptr;
      std::shared_ptr<CompactionFilterFactory> compaction_filter_factory;
      CompressionType compression; CompressionType
      bottommost_compression = kDisableCompressionOption;
      CompressionOptions bottommost_compression_opts;
      CompressionOptions compression_opts;
      std::shared_ptr<const SliceTransform> prefix_extractor = nullptr;
      bool disable_auto_compactions = false;
      std::vector<DbPath> cf_paths;
      std::shared_ptr<ConcurrentTaskLimiter> compaction_thread_limiter;
      std::shared_ptr<SstPartitionerFactory> sst_partitioner_factory;
    */

    // AdvancedColumnFamily Options
    /*
      bool inplace_update_support = false;
      size_t inplace_update_num_locks = 10000;
      bool memtable_whole_key_filtering = false;
      size_t memtable_huge_page_size = 0;
      std::shared_ptr<const SliceTransform>
          memtable_insert_with_hint_prefix_extractor;
      size_t arena_block_size = 0;
      std::vector<int> max_bytes_for_level_multiplier_additional;
      uint64_t max_compaction_bytes = 0;
      CompactionStyle compaction_style = kCompactionStyleLevel;
      CompactionPri compaction_pri = kMinOverlappingRatio;
      CompactionOptionsUniversal compaction_options_universal;
      CompactionOptionsFIFO compaction_options_fifo;
      uint64_t max_sequential_skip_in_iterations = 8;
      std::shared_ptr<MemTableRepFactory> memtable_factory;
      TablePropertiesCollectorFactories table_properties_collector_factories;
      size_t max_successive_merges = 0;
      bool check_flush_compaction_key_order = true;
      bool paranoid_file_checks = false;
      bool force_consistency_checks = true;
      bool report_bg_io_stats = false;
      uint64_t ttl = 0xfffffffffffffffe;
      uint64_t periodic_compaction_seconds = 0xfffffffffffffffe;
      uint64_t sample_for_compression = 0;
      Temperature bottommost_temperature = Temperature::kUnknown;
      bool enable_blob_files = false;
      uint64_t min_blob_size = 0;
      uint64_t blob_file_size = 1ULL << 28;
      CompressionType blob_compression_type = kNoCompression;
      bool enable_blob_garbage_collection = false;
      double blob_garbage_collection_age_cutoff = 0.25;
      double blob_garbage_collection_force_threshold = 1.0;
      uint64_t blob_compaction_readahead_size = 0;
    */
  };

  struct DBOptions {
    uint32_t numStripes;
    int64_t transactionLockTimeout;

    // TransactionDBOptions
    /*
      int64_t max_num_locks = -1;
      uint32_t max_num_deadlocks = kInitialMaxDeadlocks;
      int64_t default_lock_timeout = 1000;  // 1 second
      std::shared_ptr<TransactionDBMutexFactory> custom_mutex_factory;
      TxnDBWritePolicy write_policy = TxnDBWritePolicy::WRITE_COMMITTED;
      bool rollback_merge_operands = false;
      std::shared_ptr<LockManagerHandle> lock_mgr_handle;
      bool skip_concurrency_control = false;
      int64_t default_write_batch_flush_threshold = 0;
    */

    template<class Inspector>
    inline friend auto inspect(Inspector& f, DBOptions& o) {
      return f.object(o).fields(
          f.field("numStripes", o.numStripes),
          f.field("transactionLockTimeout", o.transactionLockTimeout));
    }
  };

  struct TableOptions {
    struct LruCacheOptions {
      uint64_t blockCacheSize;
      int64_t blockCacheShardBits{-1};
      bool enforceBlockCacheSizeLimit{true};

      template<class Inspector>
      inline friend auto inspect(Inspector& f, LruCacheOptions& o) {
        return f.object(o).fields(
            f.field("blockCacheSize", o.blockCacheSize),
            f.field("blockCacheShardBits", o.blockCacheShardBits),
            f.field("enforceBlockCacheSizeLimit",
                    o.enforceBlockCacheSizeLimit));
      }
    };

    // blockCache;

    bool cacheIndexAndFilterBlocks;
    bool cacheIndexAndFilterBlocksWithHighPriority;
    bool pinl0FilterAndIndexBlocksInCache;
    bool pinTopLevelIndexAndFilter;

    uint64_t blockSize;

    struct BloomFilterPolicy {
      double bitsPerKey{10};
      bool useBlockBasedBuilder{true};

      template<class Inspector>
      inline friend auto inspect(Inspector& f, BloomFilterPolicy& o) {
        return f.object(o).fields(
            f.field("bitsPerKey", o.bitsPerKey),
            f.field("useBlockBasedBuilder", o.useBlockBasedBuilder));
      }
    };
    // filterPolicy;

    uint32_t formatVersion;
    bool blockAlignDataBlocks;
    rocksdb::ChecksumType checksum;

    template<class Inspector>
    inline friend auto inspect(Inspector& f, TableOptions& o) {
      return f.object(o).fields(
          // f.field("blockCache", o.blockCache),
          f.field("cacheIndexAndFilterBlocks", o.cacheIndexAndFilterBlocks),
          f.field("cacheIndexAndFilterBlocksWithHighPriority",
                  o.cacheIndexAndFilterBlocksWithHighPriority),
          f.field("pinl0FilterAndIndexBlocksInCache",
                  o.pinl0FilterAndIndexBlocksInCache),
          f.field("pinTopLevelIndexAndFilter", o.pinTopLevelIndexAndFilter),
          f.field("blockSize", o.blockSize),
          f.field("formatVersion", o.formatVersion),
          f.field("blockAlignDataBlocks", o.blockAlignDataBlocks)
          // f.field("checksum", o.checksum),
          // f.field("filterPolicy", o.filterPolicy),
      );
    }
  };

  template<class Inspector>
  inline friend auto inspect(Inspector& f, RocksDBOptions& o) {
    return f.object(o).fields(
        f.field("general", o._options).fallback(f.keep()),
        f.field("db", o._dbOptions).fallback(f.keep()),
        f.field("table", o._tableOptions).fallback(f.keep()));
  }

 private:
  DBOptions _dbOptions;
  TableOptions _tableOptions;
  GeneralOptions _options;
};

}  // namespace arangodb::sepp
