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
/// @author Jan Steemann
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBEngine.h"
#include "Agency/AgencyFeature.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/LanguageFeature.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/NumberOfCores.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/RocksDBLogger.h"
#include "Basics/StaticStrings.h"
#include "Basics/Thread.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/application-exit.h"
#include "Basics/build.h"
#include "Basics/exitcodes.h"
#include "Basics/files.h"
#include "Basics/system-functions.h"
#include "Cache/Cache.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Manager.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "IResearch/IResearchCommon.h"
#include "Inspection/VPack.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/Metric.h"
#include "Metrics/MetricsFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Replication/ReplicationClients.h"
#include "Replication2/ReplicatedLog/ReplicatedLogFeature.h"
#include "Replication2/Storage/IStorageEngineMethods.h"
#include "Rest/Version.h"
#include "RestHandler/RestHandlerCreator.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/DumpLimitsFeature.h"
#include "RestServer/LanguageCheckFeature.h"
#include "RestServer/VectorIndexFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "Replication2/Storage/LogStorageMethods.h"
#include "Replication2/Storage/RocksDB/AsyncLogWriteBatcher.h"
#include "Replication2/Storage/RocksDB/AsyncLogWriteBatcherMetrics.h"
#include "Replication2/Storage/RocksDB/LogPersistor.h"
#include "Replication2/Storage/RocksDB/StatePersistor.h"
#include "Replication2/Storage/RocksDB/Metrics.h"
#include "Replication2/Storage/RocksDB/ReplicatedStateInfo.h"
#include "Replication2/Storage/WAL/IFileManager.h"
#include "Replication2/Storage/WAL/IFileWriter.h"
#include "Replication2/Storage/WAL/LogPersistor.h"
#include "Replication2/Storage/WAL/WalManager.h"
#include "RocksDBEngine/Listeners/RocksDBBackgroundErrorListener.h"
#include "RocksDBEngine/Listeners/RocksDBMetricsListener.h"
#include "RocksDBEngine/Listeners/RocksDBThrottle.h"
#include "RocksDBEngine/ReplicatedRocksDBTransactionState.h"
#include "RocksDBEngine/RocksDBBackgroundThread.h"
#include "RocksDBEngine/RocksDBChecksumEnv.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBDumpManager.h"
#include "RocksDBEngine/RocksDBFormat.h"
#include "RocksDBEngine/RocksDBIncrementalSync.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "RocksDBEngine/RocksDBIndexCacheRefillFeature.h"
#include "RocksDBEngine/RocksDBIndexFactory.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBOptimizerRules.h"
#include "RocksDBEngine/RocksDBOptionFeature.h"
#include "RocksDBEngine/RocksDBRecoveryManager.h"
#include "RocksDBEngine/RocksDBReplicationManager.h"
#include "RocksDBEngine/RocksDBReplicationTailing.h"
#include "RocksDBEngine/RocksDBRestHandlers.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBChecksumEnv.h"
#include "RocksDBEngine/RocksDBSyncThread.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "RocksDBEngine/RocksDBUpgrade.h"
#include "RocksDBEngine/RocksDBV8Functions.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "RocksDBEngine/RocksDBWalAccess.h"
#include "RocksDBEngine/SimpleRocksDBTransactionState.h"
#include "Scheduler/SchedulerFeature.h"
#include "Transaction/Context.h"
#include "Transaction/Manager.h"
#include "Transaction/Options.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalView.h"
#include "VocBase/VocbaseInfo.h"
#include "VocBase/VocBaseLogManager.h"
#include "VocBase/ticks.h"

#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>

#include <absl/strings/str_cat.h>

#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>

#include <iomanip>
#include <limits>
#include <utility>

// #define USE_CUSTOM_WAL

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::options;

namespace arangodb {

DECLARE_GAUGE(rocksdb_wal_released_tick_flush, uint64_t,
              "Released tick for RocksDB WAL deletion (flush-induced)");
DECLARE_GAUGE(rocksdb_wal_sequence, uint64_t, "Current RocksDB WAL sequence");
DECLARE_GAUGE(
    rocksdb_wal_sequence_lower_bound, uint64_t,
    "RocksDB WAL sequence number until which background thread has caught up");
DECLARE_GAUGE(rocksdb_live_wal_files, uint64_t,
              "Number of live RocksDB WAL files");
DECLARE_GAUGE(rocksdb_live_wal_files_size, uint64_t,
              "Cumulated size of live RocksDB WAL files");
DECLARE_GAUGE(rocksdb_archived_wal_files, uint64_t,
              "Number of archived RocksDB WAL files");
DECLARE_GAUGE(rocksdb_archived_wal_files_size, uint64_t,
              "Cumulated size of archived RocksDB WAL files");
DECLARE_GAUGE(rocksdb_prunable_wal_files, uint64_t,
              "Number of prunable RocksDB WAL files");
DECLARE_GAUGE(rocksdb_wal_pruning_active, uint64_t,
              "Whether or not RocksDB WAL file pruning is active");
DECLARE_GAUGE(arangodb_revision_tree_memory_usage, uint64_t,
              "Total memory consumed by all revision trees");
DECLARE_GAUGE(
    arangodb_revision_tree_buffered_memory_usage, uint64_t,
    "Total memory consumed by buffered updates for all revision trees");
DECLARE_GAUGE(arangodb_index_estimates_memory_usage, uint64_t,
              "Total memory consumed by all index selectivity estimates");
DECLARE_COUNTER(arangodb_revision_tree_rebuilds_success_total,
                "Number of successful revision tree rebuilds");
DECLARE_COUNTER(arangodb_revision_tree_rebuilds_failure_total,
                "Number of failed revision tree rebuilds");
DECLARE_COUNTER(arangodb_revision_tree_hibernations_total,
                "Number of revision tree hibernations");
DECLARE_COUNTER(arangodb_revision_tree_resurrections_total,
                "Number of revision tree resurrections");
DECLARE_COUNTER(rocksdb_cache_edge_inserts_uncompressed_entries_size_total,
                "Total gross memory size of all edge cache entries ever stored "
                "in memory");
DECLARE_COUNTER(rocksdb_cache_edge_inserts_effective_entries_size_total,
                "Total effective memory size of all edge cache entries ever "
                "stored in memory (after compression)");
DECLARE_GAUGE(rocksdb_cache_edge_compression_ratio, double,
              "Overall compression ratio for all edge cache entries ever "
              "stored in memory");
DECLARE_COUNTER(rocksdb_cache_edge_inserts_total,
                "Number of inserts into the edge cache");
DECLARE_COUNTER(rocksdb_cache_edge_compressed_inserts_total,
                "Number of compressed inserts into the edge cache");
DECLARE_COUNTER(
    rocksdb_cache_edge_empty_inserts_total,
    "Number of inserts into the edge cache that were an empty array");

// global flag to cancel all compactions. will be flipped to true on shutdown
static std::atomic<bool> cancelCompactions{false};

// minimum value for --rocksdb.sync-interval (in ms)
// a value of 0 however means turning off the syncing altogether!
static constexpr uint64_t minSyncInterval = 5;

static constexpr uint64_t databaseIdForGlobalApplier = 0;

// handles for recovery helpers
std::vector<std::shared_ptr<RocksDBRecoveryHelper>>
    RocksDBEngine::_recoveryHelpers;

RocksDBFilePurgePreventer::RocksDBFilePurgePreventer(RocksDBEngine* engine)
    : _engine(engine) {
  TRI_ASSERT(_engine != nullptr);
  _engine->_purgeLock.lockRead();
}

RocksDBFilePurgePreventer::~RocksDBFilePurgePreventer() {
  if (_engine != nullptr) {
    _engine->_purgeLock.unlockRead();
  }
}

RocksDBFilePurgePreventer::RocksDBFilePurgePreventer(
    RocksDBFilePurgePreventer&& other)
    : _engine(other._engine) {
  // steal engine from other
  other._engine = nullptr;
}

RocksDBFilePurgeEnabler::RocksDBFilePurgeEnabler(RocksDBEngine* engine)
    : _engine(nullptr) {
  TRI_ASSERT(engine != nullptr);

  if (engine->_purgeLock.tryLockWrite()) {
    // we got the lock
    _engine = engine;
  }
}

RocksDBFilePurgeEnabler::~RocksDBFilePurgeEnabler() {
  if (_engine != nullptr) {
    _engine->_purgeLock.unlockWrite();
  }
}

RocksDBFilePurgeEnabler::RocksDBFilePurgeEnabler(
    RocksDBFilePurgeEnabler&& other)
    : _engine(other._engine) {
  // steal engine from other
  other._engine = nullptr;
}

// create the storage engine
RocksDBEngine::RocksDBEngine(Server& server,
                             RocksDBOptionsProvider const& optionsProvider,
                             metrics::MetricsFeature& metrics)
    : StorageEngine(server, kEngineName, name(), Server::id<RocksDBEngine>(),
                    std::make_unique<RocksDBIndexFactory>(server)),
      _optionsProvider(optionsProvider),
      _metrics(metrics),
      _db(nullptr),
      _walAccess(std::make_unique<RocksDBWalAccess>(*this)),
      _maxTransactionSize(transaction::Options::defaultMaxTransactionSize),
      _intermediateCommitSize(
          transaction::Options::defaultIntermediateCommitSize),
      _intermediateCommitCount(
          transaction::Options::defaultIntermediateCommitCount),
      _maxParallelCompactions(2),
      _pruneWaitTime(10.0),
      _pruneWaitTimeInitial(60.0),
      _maxWalArchiveSizeLimit(0),
      _releasedTick(0),
      _syncInterval(100),
      _syncDelayThreshold(5000),
      _requiredDiskFreePercentage(0.01),
      _requiredDiskFreeBytes(16 * 1024 * 1024),
      _useThrottle(true),
      _useReleasedTick(false),
      _debugLogging(false),
      _verifySst(false),
#ifdef USE_ENTERPRISE
      _createShaFiles(true),
#else
      _createShaFiles(false),
#endif
      _lastHealthCheckSuccessful(false),
      _dbExisted(false),
      _runningRebuilds(0),
      _runningCompactions(0),
      _autoFlushCheckInterval(60.0 * 30.0),
      _autoFlushMinWalFiles(20),
      _forceLittleEndianKeys(false),
      _metricsIndexEstimatorMemoryUsage(
          metrics.add(arangodb_index_estimates_memory_usage{})),
      _metricsWalReleasedTickFlush(
          metrics.add(rocksdb_wal_released_tick_flush{})),
      _metricsWalSequenceLowerBound(
          metrics.add(rocksdb_wal_sequence_lower_bound{})),
      _metricsLiveWalFiles(metrics.add(rocksdb_live_wal_files{})),
      _metricsArchivedWalFiles(metrics.add(rocksdb_archived_wal_files{})),
      _metricsLiveWalFilesSize(metrics.add(rocksdb_live_wal_files_size{})),
      _metricsArchivedWalFilesSize(
          metrics.add(rocksdb_archived_wal_files_size{})),
      _metricsPrunableWalFiles(metrics.add(rocksdb_prunable_wal_files{})),
      _metricsWalPruningActive(metrics.add(rocksdb_wal_pruning_active{})),
      _metricsTreeMemoryUsage(
          metrics.add(arangodb_revision_tree_memory_usage{})),
      _metricsTreeBufferedMemoryUsage(
          metrics.add(arangodb_revision_tree_buffered_memory_usage{})),
      _metricsTreeRebuildsSuccess(
          metrics.add(arangodb_revision_tree_rebuilds_success_total{})),
      _metricsTreeRebuildsFailure(
          metrics.add(arangodb_revision_tree_rebuilds_failure_total{})),
      _metricsTreeHibernations(
          metrics.add(arangodb_revision_tree_hibernations_total{})),
      _metricsTreeResurrections(
          metrics.add(arangodb_revision_tree_resurrections_total{})),
      _metricsEdgeCacheEntriesSizeInitial(metrics.add(
          rocksdb_cache_edge_inserts_uncompressed_entries_size_total{})),
      _metricsEdgeCacheEntriesSizeEffective(metrics.add(
          rocksdb_cache_edge_inserts_effective_entries_size_total{})),
      _metricsEdgeCacheInserts(metrics.add(rocksdb_cache_edge_inserts_total{})),
      _metricsEdgeCacheCompressedInserts(
          metrics.add(rocksdb_cache_edge_compressed_inserts_total{})),
      _metricsEdgeCacheEmptyInserts(
          metrics.add(rocksdb_cache_edge_empty_inserts_total{})),
      _forceLegacySortingMethod(false),
      _sortingMethod(
          arangodb::basics::VelocyPackHelper::SortingMethod::Correct) {
  startsAfter<BasicFeaturePhaseServer>();
  startsAfter<VectorIndexFeature>();
  // inherits order from StorageEngine but requires "RocksDBOption" that is
  // used to configure this engine
  startsAfter<RocksDBOptionFeature>();
  startsAfter<LanguageFeature>();
  startsAfter<LanguageCheckFeature>();
}

RocksDBEngine::~RocksDBEngine() {
  _recoveryHelpers.clear();
  shutdownRocksDBInstance();
}

/// shuts down the RocksDB instance. this is called from unprepare
/// and the dtor
void RocksDBEngine::shutdownRocksDBInstance() noexcept {
  if (_db == nullptr) {
    return;
  }

  // turn off RocksDBThrottle, and release our pointers to it
  if (_throttleListener != nullptr) {
    _throttleListener->stopThread();
  }

  for (rocksdb::ColumnFamilyHandle* h :
       RocksDBColumnFamilyManager::allHandles()) {
    _db->DestroyColumnFamilyHandle(h);
  }

  // now prune all obsolete WAL files
  try {
    determinePrunableWalFiles(0);
    pruneWalFiles();
  } catch (...) {
    // this is allowed to go wrong on shutdown
    // we must not throw an exception from here
  }

  try {
    // do a final WAL sync here before shutting down
    Result res = RocksDBSyncThread::sync(_db->GetBaseDB());
    if (res.fail()) {
      LOG_TOPIC("14ede", WARN, Logger::ENGINES)
          << "could not sync RocksDB WAL: " << res.errorMessage();
    }

    rocksdb::Status status = _db->Close();

    if (!status.ok()) {
      Result res = rocksutils::convertStatus(status);
      LOG_TOPIC("2b9c1", ERR, Logger::ENGINES)
          << "could not shutdown RocksDB: " << res.errorMessage();
    }
  } catch (...) {
    // this is allowed to go wrong on shutdown
    // we must not throw an exception from here
  }

  delete _db;
  _db = nullptr;
}

void RocksDBEngine::flushOpenFilesIfRequired() {
  if (_metricsLiveWalFiles.load() < _autoFlushMinWalFiles) {
    return;
  }

  auto now = std::chrono::steady_clock::now();
  if (_autoFlushLastExecuted.time_since_epoch().count() == 0 ||
      (now - _autoFlushLastExecuted) >=
          std::chrono::duration<double>(_autoFlushCheckInterval)) {
    LOG_TOPIC("9bf16", INFO, Logger::ENGINES)
        << "auto flushing RocksDB wal and column families because number of "
           "live WAL files is "
        << _metricsLiveWalFiles.load();
    Result res = flushWal(/*waitForSync*/ true, /*flushColumnFamilies*/ true);
    if (res.fail()) {
      LOG_TOPIC("e936c", WARN, Logger::ENGINES)
          << "unable to flush RocksDB wal: " << res.errorMessage();
    }
    // set _autoFlushLastExecuted regardless of whether flushing has worked or
    // not. we don't want to put too much stress onto the db
    _autoFlushLastExecuted = now;
  }
}

// inherited from ApplicationFeature
// ---------------------------------

// add the storage engine's specific options to the global list of options
void RocksDBEngine::collectOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  options->addSection("rocksdb", "RocksDB engine");

  /// @brief minimum required percentage of free disk space for considering
  /// the server "healthy". this is expressed as a floating point value
  /// between 0 and 1! if set to 0.0, the % amount of free disk is ignored in
  /// checks.
  options
      ->addOption(
          "--rocksdb.minimum-disk-free-percent",
          "The minimum percentage of free disk space for considering the "
          "server healthy in health checks (0 = disable the check).",
          new DoubleParameter(&_requiredDiskFreePercentage, /*base*/ 1.0,
                              /*minValue*/ 0.0, /*maxValue*/ 1.0),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30800);

  /// @brief minimum number of free bytes on disk for considering the server
  /// healthy. if set to 0, the number of free bytes on disk is ignored in
  /// checks.
  options
      ->addOption("--rocksdb.minimum-disk-free-bytes",
                  "The minimum number of free disk bytes for considering the "
                  "server healthy in health checks (0 = disable the check).",
                  new UInt64Parameter(&_requiredDiskFreeBytes),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(30800);

  // control transaction size for RocksDB engine
  options
      ->addOption("--rocksdb.max-transaction-size",
                  "The transaction size limit (in bytes).",
                  new UInt64Parameter(&_maxTransactionSize))
      .setLongDescription(R"(Transactions store all keys and values in RAM, so
large transactions run the risk of causing out-of-memory situations. This
setting allows you to ensure that it does not happen by limiting the size of
any individual transaction. Transactions whose operations would consume more
RAM than this threshold value are aborted automatically with error 32
("resource limit exceeded").)");

  options->addOption("--rocksdb.intermediate-commit-size",
                     "An intermediate commit is performed automatically "
                     "when a transaction has accumulated operations of this "
                     "size (in bytes), and a new transaction is started.",
                     new UInt64Parameter(&_intermediateCommitSize));

  options->addOption("--rocksdb.intermediate-commit-count",
                     "An intermediate commit is performed automatically "
                     "when this number of operations is reached in a "
                     "transaction, and a new transaction is started.",
                     new UInt64Parameter(&_intermediateCommitCount));

  options->addOption("--rocksdb.max-parallel-compactions",
                     "The maximum number of parallel compactions jobs.",
                     new UInt64Parameter(&_maxParallelCompactions));

  options
      ->addOption(
          "--rocksdb.sync-interval",
          "The interval for automatic, non-requested disk syncs (in "
          "milliseconds, 0 = turn automatic syncing off)",
          new UInt64Parameter(&_syncInterval),
          arangodb::options::makeFlags(arangodb::options::Flags::OnDBServer,
                                       arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Automatic synchronization of data from RocksDB's
write-ahead logs to disk is only performed for not-yet synchronized data, and
only for operations that have been executed without the `waitForSync`
attribute.)");

  options->addOption(
      "--rocksdb.sync-delay-threshold",
      "The threshold for self-observation of WAL disk syncs "
      "(in milliseconds, 0 = no warnings). Any WAL disk sync longer ago "
      "than this threshold triggers a warning ",
      new UInt64Parameter(&_syncDelayThreshold),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::OnSingle,
          arangodb::options::Flags::Uncommon));

  options
      ->addOption("--rocksdb.wal-file-timeout",
                  "The timeout after which unused WAL files are deleted "
                  "(in seconds).",
                  new DoubleParameter(&_pruneWaitTime),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(Data of ongoing transactions is stored in RAM.
Transactions that get too big (in terms of number of operations involved or the
total size of data created or modified by the transaction) are committed
automatically. Effectively, this means that big user transactions are split into
multiple smaller RocksDB transactions that are committed individually.
The entire user transaction does not necessarily have ACID properties in this
case.)");

  options
      ->addOption("--rocksdb.wal-file-timeout-initial",
                  "The initial timeout (in seconds) after which unused WAL "
                  "files deletion kicks in after server start.",
                  new DoubleParameter(&_pruneWaitTimeInitial),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setLongDescription(R"(If you decrease the value, the server starts the
removal of obsolete WAL files earlier after server start. This is useful in
testing environments that are space-restricted and do not require keeping much
WAL file data at all.)");

  options
      ->addOption("--rocksdb.throttle", "Enable write-throttling.",
                  new BooleanParameter(&_useThrottle),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setLongDescription(R"(If enabled, dynamically throttles the ingest rate
of writes if necessary to reduce chances of compactions getting too far behind
and blocking incoming writes.)");

  options
      ->addOption(
          "--rocksdb.throttle-slots",
          "The number of historic metrics to use for throttle value "
          "calculation.",
          new UInt64Parameter(&_throttleSlots, /*base*/ 1, /*minValue*/ 1),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle,
              arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805)
      .setLongDescription(R"(If throttling is enabled, this parameter controls
the number of previous intervals to use for throttle value calculation.)");

  options
      ->addOption(
          "--rocksdb.throttle-frequency",
          "The frequency for write-throttle calculations (in milliseconds).",
          new UInt64Parameter(&_throttleFrequency),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle,
              arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805)
      .setLongDescription(R"(If the throttling is enabled, it recalculates a
new maximum ingestion rate with this frequency.)");

  options
      ->addOption(
          "--rocksdb.throttle-scaling-factor",
          "The adaptiveness scaling factor for write-throttle calculations.",
          new UInt64Parameter(&_throttleScalingFactor),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle,
              arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805)
      .setLongDescription(R"(There is normally no need to change this value.)");

  options
      ->addOption("--rocksdb.throttle-max-write-rate",
                  "The maximum write rate enforced by throttle (in bytes per "
                  "second, 0 = unlimited).",
                  new UInt64Parameter(&_throttleMaxWriteRate),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805)
      .setLongDescription(R"(The actual write rate established by the
throttling is the minimum of this value and the value that the regular throttle
calculation produces, i.e. this option can be used to set a fixed upper bound
on the write rate.)");

  options
      ->addOption("--rocksdb.throttle-slow-down-writes-trigger",
                  "The number of level 0 files whose payload "
                  "is not considered in throttle calculations when penalizing "
                  "the presence of L0 files.",
                  new UInt64Parameter(&_throttleSlowdownWritesTrigger),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805)
      .setLongDescription(R"(There is normally no need to change this value.)");

  options
      ->addOption("--rocksdb.throttle-lower-bound-bps",
                  "The lower bound for throttle's write bandwidth "
                  "(in bytes per second).",
                  new UInt64Parameter(&_throttleLowerBoundBps),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(30805);

#ifdef USE_ENTERPRISE
  options->addOption("--rocksdb.create-sha-files",
                     "Whether to enable the generation of sha256 files for "
                     "each .sst file.",
                     new BooleanParameter(&_createShaFiles),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnDBServer,
                         arangodb::options::Flags::OnSingle,
                         arangodb::options::Flags::Enterprise));
#endif

  // range deletes are now always enabled
  options->addObsoleteOption(
      "--rocksdb.use-range-delete-in-wal",
      "Enable range delete markers in the write-ahead log (WAL).", false);

  options
      ->addOption("--rocksdb.debug-logging",
                  "Whether to enable RocksDB debug logging.",
                  new BooleanParameter(&_debugLogging),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setLongDescription(R"(If set to `true`, enables verbose logging of
RocksDB's actions into the logfile written by ArangoDB (if the
`--rocksdb.use-file-logging` option is off), or RocksDB's own log (if the
`--rocksdb.use-file-logging` option is on).

This option is turned off by default, but you can enable it for debugging
RocksDB internals and performance.)");

  options->addObsoleteOption("--rocksdb.edge-cache",
                             "Whether to use the in-memory cache for edges",
                             false);

  options
      ->addOption("--rocksdb.verify-sst",
                  "Verify the validity of .sst files present in the "
                  "`engine-rocksdb` directory on startup.",
                  new BooleanParameter(&_verifySst),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Command,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnAgent,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31100)
      .setLongDescription(R"(If set to `true`, during startup, all .sst files
in the `engine-rocksdb` folder in the database directory are checked for
potential corruption and errors. The server process stops after the check and
returns an exit code of `0` if the validation was successful, or a non-zero
exit code if there is an error in any of the .sst files.)");

  options
      ->addOption("--rocksdb.wal-archive-size-limit",
                  "The maximum total size (in bytes) of archived WAL files to "
                  "keep on the leader (0 = unlimited).",
                  new UInt64Parameter(&_maxWalArchiveSizeLimit),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle,
                      arangodb::options::Flags::Uncommon))
      .setDeprecatedIn(31200)
      .setLongDescription(R"(A value of `0` does not restrict the size of the
archive, so the leader removes archived WAL files when there are no replication
clients needing them. Any non-zero value restricts the size of the WAL files
archive to about the specified value and trigger WAL archive file deletion once
the threshold is reached. You can use this to get rid of archived WAL files in
a disk size-constrained environment.

**Note**: The value is only a threshold, so the archive may get bigger than 
the configured value until the background thread actually deletes files from
the archive. Also note that deletion from the archive only kicks in after
`--rocksdb.wal-file-timeout-initial` seconds have elapsed after server start.

Archived WAL files are normally deleted automatically after a short while when
there is no follower attached that may read from the archive. However, in case
when there are followers attached that may read from the archive, WAL files
normally remain in the archive until their contents have been streamed to the
followers. In case there are slow followers that cannot catch up, this causes a
growth of the WAL files archive over time.

You can use the option to force a deletion of WAL files from the archive even if
there are followers attached that may want to read the archive. In case the
option is set and a leader deletes files from the archive that followers want to
read, this aborts the replication on the followers. Followers can restart the
replication doing a resync, though, but they may not be able to catch up if WAL
file deletion happens too early.

Thus it is best to leave this option at its default value of `0` except in cases
when disk size is very constrained and no replication is used.)");

  options
      ->addOption("--rocksdb.auto-flush-min-live-wal-files",
                  "The minimum number of live WAL files that triggers an "
                  "auto-flush of WAL "
                  "and column family data.",
                  new UInt64Parameter(&_autoFlushMinWalFiles),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31005);

  options
      ->addOption(
          "--rocksdb.auto-flush-check-interval",
          "The interval (in seconds) in which auto-flushes of WAL and column "
          "family data is executed.",
          new DoubleParameter(&_autoFlushCheckInterval),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31005);

  options
      ->addOption(
          "--rocksdb.force-legacy-comparator",
          "If set to `true`, forces a new database directory to use the "
          "legacy sorting method. This is only for testing. Don't use.",
          new BooleanParameter(&_forceLegacySortingMethod),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31202);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  options
      ->addOption(
          "--rocksdb.force-legacy-little-endian-keys",
          "Force usage of legacy little endian key encoding when creating "
          "a new RocksDB database directory. DO NOT USE IN PRODUCTION.",
          new BooleanParameter(&_forceLittleEndianKeys),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::Uncommon,
              arangodb::options::Flags::Experimental,
              arangodb::options::Flags::OnAgent,
              arangodb::options::Flags::OnDBServer,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31200)
      .setLongDescription(R"(If enabled and a new RocksDB database
is generated, the legacy little endian key encoding is used.

Only use this option for testing purposes! It is bad for performance and
disables a few features like parallel index generation!)");
#endif

#ifdef USE_ENTERPRISE
  collectEnterpriseOptions(options);
#endif
}

// validate the storage engine's specific options
void RocksDBEngine::validateOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  transaction::Options::setLimits(_maxTransactionSize, _intermediateCommitSize,
                                  _intermediateCommitCount);
#ifdef USE_ENTERPRISE
  validateEnterpriseOptions(options);
#endif

  if (_throttleScalingFactor == 0) {
    _throttleScalingFactor = 1;
  }

  if (_throttleSlots < 8) {
    _throttleSlots = 8;
  }

  if (_syncInterval > 0) {
    if (_syncInterval < minSyncInterval) {
      // _syncInterval = 0 means turned off!
      LOG_TOPIC("bbd68", FATAL, arangodb::Logger::CONFIG)
          << "invalid value for --rocksdb.sync-interval. Please use a value "
          << "of at least " << minSyncInterval;
      FATAL_ERROR_EXIT();
    }

    if (_syncDelayThreshold > 0 && _syncDelayThreshold <= _syncInterval) {
      if (!options->processingResult().touched("rocksdb.sync-interval") &&
          options->processingResult().touched("rocksdb.sync-delay-threshold")) {
        // user has not set --rocksdb.sync-interval, but set
        // --rocksdb.sync-delay-threshold
        LOG_TOPIC("c3f45", WARN, arangodb::Logger::CONFIG)
            << "invalid value for --rocksdb.sync-delay-threshold. should be "
               "higher "
            << "than the value of --rocksdb.sync-interval (" << _syncInterval
            << ")";
      }

      _syncDelayThreshold = 10 * _syncInterval;
      LOG_TOPIC("c0fa3", WARN, arangodb::Logger::CONFIG)
          << "auto-adjusting value of --rocksdb.sync-delay-threshold to "
          << _syncDelayThreshold << " ms";
    }
  }

  if (_pruneWaitTimeInitial < 10) {
    LOG_TOPIC("a9667", WARN, arangodb::Logger::ENGINES)
        << "consider increasing the value for "
           "--rocksdb.wal-file-timeout-initial. "
        << "Replication clients might have trouble to get in sync";
  }
}

// preparation phase for storage engine. can be used for internal setup.
// the storage engine must not start any threads here or write any files
void RocksDBEngine::prepare() {
  // get base path from DatabaseServerFeature
  auto& databasePathFeature = server().getFeature<DatabasePathFeature>();
  _basePath = databasePathFeature.directory();

  TRI_ASSERT(!_basePath.empty());

#ifdef USE_ENTERPRISE
  prepareEnterprise();
#endif
}

void RocksDBEngine::verifySstFiles(rocksdb::Options const& options) const {
  TRI_ASSERT(!_path.empty());

  LOG_TOPIC("e210d", INFO, arangodb::Logger::STARTUP)
      << "verifying RocksDB .sst files in path '" << _path << "'";

  rocksdb::SstFileReader sstReader(options);
  for (auto const& fileName : TRI_FullTreeDirectory(_path.c_str())) {
    if (!fileName.ends_with(".sst")) {
      continue;
    }
    std::string filename = basics::FileUtils::buildFilename(_path, fileName);
    rocksdb::Status res = sstReader.Open(filename);
    if (res.ok()) {
      res = sstReader.VerifyChecksum();
    }
    if (!res.ok()) {
      auto result = rocksutils::convertStatus(res);
      LOG_TOPIC("2943c", FATAL, arangodb::Logger::STARTUP)
          << "error when verifying .sst file '" << filename
          << "': " << result.errorMessage();
      FATAL_ERROR_EXIT_CODE(TRI_EXIT_SST_FILE_CHECK);
    }
  }

  LOG_TOPIC("02224", INFO, arangodb::Logger::STARTUP)
      << "verification of RocksDB .sst files in path '" << _path
      << "' completed successfully";
  Logger::flush();
  // exit with status code = 0, without leaking
  int exitCode = static_cast<int>(TRI_ERROR_NO_ERROR);
  TRI_EXIT_FUNCTION(exitCode, nullptr);
  exit(exitCode);
}

bool RocksDBEngine::isVectorIndexEnabled() const {
  return server().getFeature<VectorIndexFeature>().isVectorIndexEnabled();
}

namespace {

struct RocksDBAsyncLogWriteBatcherMetricsImpl
    : replication2::storage::rocksdb::AsyncLogWriteBatcherMetrics {
  explicit RocksDBAsyncLogWriteBatcherMetricsImpl(
      metrics::MetricsFeature* metricsFeature) {
    using namespace arangodb::replication2::storage::rocksdb;
    numWorkerThreadsWaitForSync = &metricsFeature->add(
        arangodb_replication2_rocksdb_num_persistor_worker{}.withLabel("ws",
                                                                       "true"));
    numWorkerThreadsNoWaitForSync = &metricsFeature->add(
        arangodb_replication2_rocksdb_num_persistor_worker{}.withLabel(
            "ws", "false"));

    queueLength =
        &metricsFeature->add(arangodb_replication2_rocksdb_queue_length{});
    writeBatchSize =
        &metricsFeature->add(arangodb_replication2_rocksdb_write_batch_size{});
    rocksdbWriteTimeInUs =
        &metricsFeature->add(arangodb_replication2_rocksdb_write_time{});
    rocksdbSyncTimeInUs =
        &metricsFeature->add(arangodb_replication2_rocksdb_sync_time{});

    operationLatencyInsert = &metricsFeature->add(
        arangodb_replication2_storage_operation_latency{}.withLabel("op",
                                                                    "insert"));
    operationLatencyRemoveFront = &metricsFeature->add(
        arangodb_replication2_storage_operation_latency{}.withLabel(
            "op", "remove-front"));
    operationLatencyRemoveBack = &metricsFeature->add(
        arangodb_replication2_storage_operation_latency{}.withLabel(
            "op", "remove-back"));
  }
};
}  // namespace

void RocksDBEngine::start() {
  // it is already decided that rocksdb is used
  TRI_ASSERT(isEnabled());
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  if (ServerState::instance()->isAgent() &&
      !server().options()->processingResult().touched(
          "rocksdb.wal-file-timeout-initial")) {
    // reduce --rocksb.wal-file-timeout-initial to 15 seconds for agency nodes
    // as we probably won't need the WAL for WAL tailing and replication here
    _pruneWaitTimeInitial = 15;
  }

  LOG_TOPIC("107fd", TRACE, arangodb::Logger::ENGINES)
      << "rocksdb version " << rest::Version::getRocksDBVersion()
      << ", supported compression types: " << getCompressionSupport();

  // set the database sub-directory for RocksDB
  auto& databasePathFeature = server().getFeature<DatabasePathFeature>();
  _path = databasePathFeature.subdirectoryName("engine-rocksdb");

  [[maybe_unused]] bool createdEngineDir = false;
  if (!basics::FileUtils::isDirectory(_path)) {
    std::string systemErrorStr;
    long errorNo;

    auto res =
        TRI_CreateRecursiveDirectory(_path.c_str(), errorNo, systemErrorStr);

    if (res == TRI_ERROR_NO_ERROR) {
      LOG_TOPIC("b2958", TRACE, arangodb::Logger::ENGINES)
          << "created RocksDB data directory '" << _path << "'";
      createdEngineDir = true;
    } else {
      LOG_TOPIC("a5ae3", FATAL, arangodb::Logger::ENGINES)
          << "unable to create RocksDB data directory '" << _path
          << "': " << systemErrorStr;
      FATAL_ERROR_EXIT();
    }
    // When we are getting here, the whole engine-rocksdb directory
    // did not exist when we got here. Therefore, we can use the correct
    // sorting behaviour in the RocksDBVPackComparator:
    _sortingMethod =
        _forceLegacySortingMethod
            ? arangodb::basics::VelocyPackHelper::SortingMethod::Legacy
            : arangodb::basics::VelocyPackHelper::SortingMethod::Correct;
    // Now remember this decision by putting a `SORTING` file in the
    // database directory:
    if (writeSortingFile(_sortingMethod).fail()) {
      FATAL_ERROR_EXIT();
    }
  } else {
    // In this case the engine-rocksdb directory does already exist.
    // Therefore, we want to recognize the sorting behaviour in the
    // RocksDBVPackComparator:
    _sortingMethod = readSortingFile();
  }
  // Now fix the actual rocksdb Comparator, if we need to:
  if (_sortingMethod ==
      arangodb::basics::VelocyPackHelper::SortingMethod::Legacy) {
    LOG_TOPIC("12653", WARN, Logger::ENGINES)
        << "ATTENTION: Using legacy sorting method for VPack indexes. Consider "
           "running GET /_admin/cluster/vpackSortMigration/check to find out "
           "if cheap migration is an option.";
    server().getFeature<RocksDBOptionFeature>().resetVPackComparator(
        std::make_unique<RocksDBVPackComparator<
            arangodb::basics::VelocyPackHelper::SortingMethod::Legacy>>());
  }

  _idxPath = basics::FileUtils::buildFilename(_path, "tmp-idx-creation");
  if (basics::FileUtils::isDirectory(_idxPath)) {
    for (auto const& fileName : TRI_FullTreeDirectory(_idxPath.c_str())) {
      TRI_UnlinkFile(
          basics::FileUtils::buildFilename(_idxPath, fileName).c_str());
    }
  } else {
    auto errorMsg = TRI_ERROR_NO_ERROR;
    if (!basics::FileUtils::createDirectory(_idxPath, &errorMsg)) {
      LOG_TOPIC("6d10f", FATAL, Logger::ENGINES)
          << "Cannot create tmp-idx-creation directory: " << TRI_last_error();
      FATAL_ERROR_EXIT();
    }
  }

  uint64_t totalSpace;
  uint64_t freeSpace;
  if (TRI_GetDiskSpaceInfo(_path, totalSpace, freeSpace).ok() &&
      totalSpace != 0) {
    LOG_TOPIC("b71b9", DEBUG, arangodb::Logger::ENGINES)
        << "total disk space for database directory mount: "
        << basics::StringUtils::formatSize(totalSpace)
        << ", free disk space for database directory mount: "
        << basics::StringUtils::formatSize(freeSpace) << " ("
        << (100.0 * double(freeSpace) / double(totalSpace)) << "% free)";
  }

  rocksdb::TransactionDBOptions transactionOptions =
      _optionsProvider.getTransactionDBOptions();

  // we only want to modify DBOptions here, no ColumnFamily options or the
  // like
  _dbOptions = _optionsProvider.getOptions();
  if (_dbOptions.wal_dir.empty()) {
    _dbOptions.wal_dir = basics::FileUtils::buildFilename(_path, "journals");
  }
  LOG_TOPIC("bc82a", TRACE, arangodb::Logger::ENGINES)
      << "initializing RocksDB, path: '" << _path << "', WAL directory '"
      << _dbOptions.wal_dir << "'";

  if (_verifySst) {
    // startup command was invoked to verify all .sst files
    rocksdb::Options options;
#ifdef USE_ENTERPRISE
    configureEnterpriseRocksDBOptions(options, createdEngineDir);
#else
    options.env = rocksdb::Env::Default();
#endif
    // we will not return from here
    verifySstFiles(options);
    TRI_ASSERT(false);
  }

  if (_createShaFiles) {
    _checksumEnv =
        std::make_unique<checksum::ChecksumEnv>(rocksdb::Env::Default(), _path);
    _dbOptions.env = _checksumEnv.get();
    static_cast<checksum::ChecksumEnv*>(_checksumEnv.get())
        ->getHelper()
        ->checkMissingShaFiles();  // this works even if done before
                                   // configureEnterpriseRocksDBOptions() is
                                   // called when there's encryption, because
                                   // checkMissingShafiles() only looks for
                                   // existing sst files without their sha
                                   // files in the directory and writes the
                                   // missing sha files.
  } else {
    _dbOptions.env = rocksdb::Env::Default();
  }

#ifdef USE_ENTERPRISE
  configureEnterpriseRocksDBOptions(_dbOptions, createdEngineDir);
#endif

  _dbOptions.env->SetBackgroundThreads(
      static_cast<int>(_optionsProvider.numThreadsHigh()),
      rocksdb::Env::Priority::HIGH);
  _dbOptions.env->SetBackgroundThreads(
      static_cast<int>(_optionsProvider.numThreadsLow()),
      rocksdb::Env::Priority::LOW);

  if (_debugLogging) {
    _dbOptions.info_log_level = rocksdb::InfoLogLevel::DEBUG_LEVEL;
  }

  std::shared_ptr<RocksDBLogger> logger;

  if (!_optionsProvider.useFileLogging()) {
    // if option "--rocksdb.use-file-logging" is set to false, we will use
    // our own logger that logs to ArangoDB's logfile
    logger = std::make_shared<RocksDBLogger>(_dbOptions.info_log_level);
    _dbOptions.info_log = logger;

    if (!_debugLogging) {
      logger->disable();
    }
  }

  if (_useThrottle) {
    _throttleListener = std::make_shared<RocksDBThrottle>(
        _throttleSlots, _throttleFrequency, _throttleScalingFactor,
        _throttleMaxWriteRate, _throttleSlowdownWritesTrigger,
        _throttleLowerBoundBps);
    _dbOptions.listeners.push_back(_throttleListener);
  }

  _errorListener = std::make_shared<RocksDBBackgroundErrorListener>();
  _dbOptions.listeners.push_back(_errorListener);
  _dbOptions.listeners.push_back(
      std::make_shared<RocksDBMetricsListener>(server()));

  rocksdb::BlockBasedTableOptions tableOptions =
      _optionsProvider.getTableOptions();
  // create column families
  std::vector<rocksdb::ColumnFamilyDescriptor> cfFamilies;
  auto addFamily = [this,
                    &cfFamilies](RocksDBColumnFamilyManager::Family family) {
    rocksdb::ColumnFamilyOptions specialized =
        _optionsProvider.getColumnFamilyOptions(family);
    std::string name = RocksDBColumnFamilyManager::name(family);
    cfFamilies.emplace_back(name, specialized);
  };
  // no prefix families for default column family (Has to be there)
  addFamily(RocksDBColumnFamilyManager::Family::Definitions);
  addFamily(RocksDBColumnFamilyManager::Family::Documents);
  addFamily(RocksDBColumnFamilyManager::Family::PrimaryIndex);
  addFamily(RocksDBColumnFamilyManager::Family::EdgeIndex);
  addFamily(RocksDBColumnFamilyManager::Family::VPackIndex);
  addFamily(RocksDBColumnFamilyManager::Family::GeoIndex);
  addFamily(RocksDBColumnFamilyManager::Family::FulltextIndex);
  addFamily(RocksDBColumnFamilyManager::Family::ReplicatedLogs);
  addFamily(RocksDBColumnFamilyManager::Family::MdiIndex);
  addFamily(RocksDBColumnFamilyManager::Family::MdiVPackIndex);
  if (isVectorIndexEnabled()) {
    addFamily(RocksDBColumnFamilyManager::Family::VectorIndex);
  }

  bool dbExisted = checkExistingDB(cfFamilies);

  LOG_TOPIC("ab45b", DEBUG, Logger::STARTUP)
      << "opening RocksDB instance in '" << _path << "'";

  std::vector<rocksdb::ColumnFamilyHandle*> cfHandles;

  rocksdb::Status status = rocksdb::TransactionDB::Open(
      _dbOptions, transactionOptions, _path, cfFamilies, &cfHandles, &_db);

  if (!status.ok()) {
    std::string error;
    if (status.IsIOError()) {
      error =
          "; Maybe your filesystem doesn't provide required features? (Cifs? "
          "NFS?)";
    }

    LOG_TOPIC("fe3df", FATAL, arangodb::Logger::STARTUP)
        << "unable to initialize RocksDB engine: " << status.ToString()
        << error;
    FATAL_ERROR_EXIT();
  }
  if (cfFamilies.size() != cfHandles.size()) {
    LOG_TOPIC("ffc6d", FATAL, arangodb::Logger::STARTUP)
        << "unable to initialize RocksDB column families";
    FATAL_ERROR_EXIT();
  }
  if (cfHandles.size() <
      RocksDBColumnFamilyManager::minNumberOfColumnFamilies) {
    LOG_TOPIC("e572e", FATAL, arangodb::Logger::STARTUP)
        << "unexpected number of column families found in database. "
        << "got " << cfHandles.size() << ", expecting at least "
        << RocksDBColumnFamilyManager::minNumberOfColumnFamilies;
    FATAL_ERROR_EXIT();
  }

  // give throttle access to families
  if (_useThrottle) {
    _throttleListener->setFamilies(cfHandles);
  }

  TRI_ASSERT(_db != nullptr);

  // set our column families
  RocksDBColumnFamilyManager::set(RocksDBColumnFamilyManager::Family::Invalid,
                                  _db->DefaultColumnFamily());
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::Definitions, cfHandles[0]);
  RocksDBColumnFamilyManager::set(RocksDBColumnFamilyManager::Family::Documents,
                                  cfHandles[1]);
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::PrimaryIndex, cfHandles[2]);
  RocksDBColumnFamilyManager::set(RocksDBColumnFamilyManager::Family::EdgeIndex,
                                  cfHandles[3]);
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::VPackIndex, cfHandles[4]);
  RocksDBColumnFamilyManager::set(RocksDBColumnFamilyManager::Family::GeoIndex,
                                  cfHandles[5]);
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::FulltextIndex, cfHandles[6]);
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::ReplicatedLogs, cfHandles[7]);
  RocksDBColumnFamilyManager::set(RocksDBColumnFamilyManager::Family::MdiIndex,
                                  cfHandles[8]);
  RocksDBColumnFamilyManager::set(
      RocksDBColumnFamilyManager::Family::MdiVPackIndex, cfHandles[9]);
  if (isVectorIndexEnabled()) {
    RocksDBColumnFamilyManager::set(
        RocksDBColumnFamilyManager::Family::VectorIndex, cfHandles[10]);
  }
  TRI_ASSERT(RocksDBColumnFamilyManager::get(
                 RocksDBColumnFamilyManager::Family::Definitions)
                 ->GetID() == 0);

  // will crash the process if version does not match
  arangodb::rocksdbStartupVersionCheck(server(), _db, dbExisted,
                                       _forceLittleEndianKeys);

  _dbExisted = dbExisted;

  // only enable logger after RocksDB start
  if (logger != nullptr) {
    logger->enable();
  }

  if (_optionsProvider.limitOpenFilesAtStartup()) {
    _db->SetDBOptions({{"max_open_files", "-1"}});
  }

  // limit the total size of WAL files. This forces the flush of memtables of
  // column families still backed by WAL files. If we would not do this, WAL
  // files may linger around forever and will not get removed
  _db->SetDBOptions({{"max_total_wal_size",
                      std::to_string(_optionsProvider.maxTotalWalSize())}});

  {
    auto& feature = server().getFeature<FlushFeature>();
    _useReleasedTick = feature.isEnabled();
  }

  // useReleasedTick should be true on DB servers and single servers
  TRI_ASSERT((arangodb::ServerState::instance()->isCoordinator() ||
              arangodb::ServerState::instance()->isAgent()) ||
             _useReleasedTick);

  if (_syncInterval > 0) {
    _syncThread = std::make_unique<RocksDBSyncThread>(
        *this, std::chrono::milliseconds(_syncInterval),
        std::chrono::milliseconds(_syncDelayThreshold));
    if (!_syncThread->start()) {
      LOG_TOPIC("63919", FATAL, Logger::ENGINES)
          << "could not start rocksdb sync thread";
      FATAL_ERROR_EXIT();
    }
  }

  TRI_ASSERT(_db != nullptr);
  _settingsManager = std::make_unique<RocksDBSettingsManager>(*this);
  _replicationManager = std::make_unique<RocksDBReplicationManager>(*this);
  _dumpManager = std::make_unique<RocksDBDumpManager>(
      *this, _metrics, server().getFeature<DumpLimitsFeature>().limits());
  _walManager = std::make_shared<replication2::storage::wal::WalManager>(
      databasePathFeature.subdirectoryName("replicated-logs"));

  struct SchedulerExecutor
      : replication2::storage::rocksdb::AsyncLogWriteBatcher::IAsyncExecutor {
    explicit SchedulerExecutor(ArangodServer& server)
        : _scheduler(server.getFeature<SchedulerFeature>().SCHEDULER) {}

    void operator()(fu2::unique_function<void() noexcept> func) override {
      _scheduler->queue(RequestLane::CLUSTER_INTERNAL, std::move(func));
    }

    Scheduler* _scheduler;
  };

  _logMetrics =
      std::make_shared<RocksDBAsyncLogWriteBatcherMetricsImpl>(&_metrics);

#ifndef USE_CUSTOM_WAL
  // When using the custom WAL implementation, we don't need to register a
  // RocksDB sync listener.
  auto logPersistor =
      std::make_shared<replication2::storage::rocksdb::AsyncLogWriteBatcher>(
          RocksDBColumnFamilyManager::get(
              RocksDBColumnFamilyManager::Family::ReplicatedLogs),
          _db->GetRootDB(), std::make_shared<SchedulerExecutor>(server()),
          server().getFeature<ReplicatedLogFeature>().options(), _logMetrics);
  _logPersistor = logPersistor;

  if (auto* syncer = syncThread(); syncer != nullptr) {
    syncer->registerSyncListener(std::move(logPersistor));
  } else {
    LOG_TOPIC("0a5df", WARN, Logger::REPLICATION2)
        << "In replication2 databases, setting waitForSync to false will not "
           "work correctly without a syncer thread. See the "
           "--rocksdb.sync-interval option.";
  }
#endif

  _settingsManager->retrieveInitialValues();

  double const counterSyncSeconds = 2.5;
  _backgroundThread =
      std::make_unique<RocksDBBackgroundThread>(*this, counterSyncSeconds);
  if (!_backgroundThread->start()) {
    LOG_TOPIC("a5e96", FATAL, Logger::ENGINES)
        << "could not start rocksdb counter manager thread";
    FATAL_ERROR_EXIT();
  }

  if (!systemDatabaseExists()) {
    addSystemDatabase();
  }

  // to populate initial health check data
  HealthData hd = healthCheck();
  if (hd.res.fail()) {
    LOG_TOPIC("4cf5b", ERR, Logger::ENGINES) << hd.res.errorMessage();
  }

  // make an initial inventory of WAL files, so that all WAL files
  // metrics are correctly populated once the HTTP interface comes
  // up
  determineWalFilesInitial();
}

void RocksDBEngine::beginShutdown() {
  TRI_ASSERT(isEnabled());

  // block the creation of new replication contexts
  if (_replicationManager != nullptr) {
    _replicationManager->beginShutdown();
  }

  if (_dumpManager != nullptr) {
    _dumpManager->garbageCollect(/*force*/ true);
  }

  // from now on, all started compactions can be canceled.
  // note that this is only a best-effort hint to RocksDB and
  // may not be followed immediately.
  ::cancelCompactions.store(true, std::memory_order_release);
}

void RocksDBEngine::stop() {
  TRI_ASSERT(isEnabled());

  // in case we missed the beginShutdown somehow, call it again
  replicationManager()->beginShutdown();
  replicationManager()->dropAll();

  if (_dumpManager != nullptr) {
    _dumpManager->garbageCollect(/*force*/ true);
  }

  if (_backgroundThread) {
    // stop the press
    _backgroundThread->beginShutdown();

    if (_settingsManager) {
      auto syncRes = _settingsManager->sync(/*force*/ true);
      if (syncRes.fail()) {
        LOG_TOPIC("0582f", WARN, Logger::ENGINES)
            << "caught exception while shutting down RocksDB engine: "
            << syncRes.errorMessage();
      }
    }

    // wait until background thread stops
    while (_backgroundThread->isRunning()) {
      std::this_thread::yield();
    }
    _backgroundThread.reset();
  }

  if (_syncThread) {
    // _syncThread may be a nullptr, in case automatic syncing is turned off
    _syncThread->beginShutdown();

    // wait until sync thread stops
    while (_syncThread->isRunning()) {
      std::this_thread::yield();
    }
    _syncThread.reset();
  }

  waitForCompactionJobsToFinish();
}

void RocksDBEngine::unprepare() {
  TRI_ASSERT(isEnabled());
  waitForCompactionJobsToFinish();
  shutdownRocksDBInstance();
}

void RocksDBEngine::trackRevisionTreeHibernation() noexcept {
  ++_metricsTreeHibernations;
}

void RocksDBEngine::trackRevisionTreeResurrection() noexcept {
  ++_metricsTreeResurrections;
}

void RocksDBEngine::trackRevisionTreeMemoryIncrease(
    std::uint64_t value) noexcept {
  _metricsTreeMemoryUsage.fetch_add(value);
}

void RocksDBEngine::trackRevisionTreeMemoryDecrease(
    std::uint64_t value) noexcept {
  [[maybe_unused]] auto old = _metricsTreeMemoryUsage.fetch_sub(value);
  TRI_ASSERT(old >= value);
}

void RocksDBEngine::trackRevisionTreeBufferedMemoryIncrease(
    std::uint64_t value) noexcept {
  _metricsTreeBufferedMemoryUsage.fetch_add(value);
}

void RocksDBEngine::trackRevisionTreeBufferedMemoryDecrease(
    std::uint64_t value) noexcept {
  [[maybe_unused]] auto old = _metricsTreeBufferedMemoryUsage.fetch_sub(value);
  TRI_ASSERT(old >= value);
}

bool RocksDBEngine::hasBackgroundError() const {
  return _errorListener != nullptr && _errorListener->called();
}

std::unique_ptr<transaction::Manager> RocksDBEngine::createTransactionManager(
    transaction::ManagerFeature& feature) {
  return std::make_unique<transaction::Manager>(feature);
}

std::shared_ptr<TransactionState> RocksDBEngine::createTransactionState(
    TRI_vocbase_t& vocbase, TransactionId tid,
    transaction::Options const& options, transaction::OperationOrigin trxType) {
  if (vocbase.replicationVersion() == replication::Version::TWO &&
      (tid.isLeaderTransactionId() || tid.isLegacyTransactionId()) &&
      ServerState::instance()->isRunningInCluster() &&
      !options.allowDirtyReads && options.requiresReplication) {
    return std::make_shared<ReplicatedRocksDBTransactionState>(
        vocbase, tid, options, trxType);
  }
  return std::make_shared<SimpleRocksDBTransactionState>(vocbase, tid, options,
                                                         trxType);
}

void RocksDBEngine::addParametersForNewCollection(VPackBuilder& builder,
                                                  VPackSlice info) {
  if (!info.hasKey(StaticStrings::ObjectId)) {
    builder.add(StaticStrings::ObjectId,
                VPackValue(std::to_string(TRI_NewTickServer())));
  }
  if (!info.get(StaticStrings::CacheEnabled).isBool()) {
    builder.add(StaticStrings::CacheEnabled, VPackValue(false));
  }
}

// create storage-engine specific collection
std::unique_ptr<PhysicalCollection> RocksDBEngine::createPhysicalCollection(
    LogicalCollection& collection, velocypack::Slice info) {
  return std::make_unique<RocksDBCollection>(collection, info);
}

// inventory functionality
// -----------------------

void RocksDBEngine::getDatabases(arangodb::velocypack::Builder& result) {
  LOG_TOPIC("a9cc7", TRACE, Logger::STARTUP) << "getting existing databases";

  rocksdb::ReadOptions readOptions;
  std::unique_ptr<rocksdb::Iterator> iter(_db->NewIterator(
      readOptions, RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::Definitions)));
  result.openArray();
  auto rSlice = rocksDBSlice(RocksDBEntryType::Database);
  for (iter->Seek(rSlice); iter->Valid() && iter->key().starts_with(rSlice);
       iter->Next()) {
    auto slice =
        VPackSlice(reinterpret_cast<uint8_t const*>(iter->value().data()));

    //// check format id
    TRI_ASSERT(slice.isObject());
    VPackSlice idSlice = slice.get(StaticStrings::DatabaseId);
    if (!idSlice.isString()) {
      LOG_TOPIC("099d7", ERR, arangodb::Logger::STARTUP)
          << "found invalid database declaration with non-string id: "
          << slice.toJson();
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE);
    }

    // deleted
    if (arangodb::basics::VelocyPackHelper::getBooleanValue(slice, "deleted",
                                                            false)) {
      TRI_voc_tick_t id = static_cast<TRI_voc_tick_t>(
          basics::StringUtils::uint64(idSlice.copyString()));

      // database is deleted, skip it!
      LOG_TOPIC("43cbc", DEBUG, arangodb::Logger::STARTUP)
          << "found dropped database " << id;

      dropDatabase(id);
      continue;
    }

    // name
    VPackSlice nameSlice = slice.get("name");
    if (!nameSlice.isString()) {
      LOG_TOPIC("96ffc", ERR, arangodb::Logger::STARTUP)
          << "found invalid database declaration with non-string name: "
          << slice.toJson();
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE);
    }

    result.add(slice);
  }
  result.close();
}

void RocksDBEngine::getCollectionInfo(TRI_vocbase_t& vocbase, DataSourceId cid,
                                      arangodb::velocypack::Builder& builder,
                                      bool includeIndexes,
                                      TRI_voc_tick_t maxTick) {
  builder.openObject();

  // read collection info from database
  RocksDBKey key;

  key.constructCollection(vocbase.id(), cid);

  rocksdb::PinnableSlice value;
  rocksdb::ReadOptions options;
  rocksdb::Status res =
      _db->Get(options,
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Definitions),
               key.string(), &value);
  auto result = rocksutils::convertStatus(res);

  if (result.fail()) {
    THROW_ARANGO_EXCEPTION(result);
  }

  VPackSlice fullParameters = RocksDBValue::data(value);

  builder.add("parameters", fullParameters);

  if (includeIndexes) {
    // dump index information
    VPackSlice indexes = fullParameters.get("indexes");
    builder.add(VPackValue("indexes"));
    builder.openArray();

    if (indexes.isArray()) {
      for (auto const idx : VPackArrayIterator(indexes)) {
        // This is only allowed to contain user-defined indexes.
        // So we have to exclude Primary + Edge Types
        auto type = idx.get(StaticStrings::IndexType);
        TRI_ASSERT(type.isString());

        if (!type.isEqualString("primary") && !type.isEqualString("edge")) {
          builder.add(idx);
        }
      }
    }

    builder.close();
  }

  builder.close();
}

ErrorCode RocksDBEngine::getCollectionsAndIndexes(
    TRI_vocbase_t& vocbase, arangodb::velocypack::Builder& result,
    bool wasCleanShutdown, bool isUpgrade) {
  rocksdb::ReadOptions readOptions;
  std::unique_ptr<rocksdb::Iterator> iter(_db->NewIterator(
      readOptions, RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::Definitions)));

  result.openArray();

  auto rSlice = rocksDBSlice(RocksDBEntryType::Collection);

  for (iter->Seek(rSlice); iter->Valid() && iter->key().starts_with(rSlice);
       iter->Next()) {
    if (vocbase.id() != RocksDBKey::databaseId(iter->key())) {
      continue;
    }

    auto slice =
        VPackSlice(reinterpret_cast<uint8_t const*>(iter->value().data()));

    if (arangodb::basics::VelocyPackHelper::getBooleanValue(
            slice, StaticStrings::DataSourceDeleted, false)) {
      continue;
    }

    result.add(slice);
  }

  result.close();

  return TRI_ERROR_NO_ERROR;
}

ErrorCode RocksDBEngine::getViews(TRI_vocbase_t& vocbase,
                                  arangodb::velocypack::Builder& result) {
  auto bounds = RocksDBKeyBounds::DatabaseViews(vocbase.id());
  rocksdb::Slice upper = bounds.end();
  rocksdb::ColumnFamilyHandle* cf = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Definitions);

  rocksdb::ReadOptions ro;
  ro.iterate_upper_bound = &upper;

  std::unique_ptr<rocksdb::Iterator> iter(_db->NewIterator(ro, cf));
  result.openArray();
  for (iter->Seek(bounds.start()); iter->Valid(); iter->Next()) {
    TRI_ASSERT(iter->key().compare(bounds.end()) < 0);
    auto slice =
        VPackSlice(reinterpret_cast<uint8_t const*>(iter->value().data()));

    LOG_TOPIC("e3bcd", TRACE, Logger::VIEWS)
        << "got view slice: " << slice.toJson();

    if (arangodb::basics::VelocyPackHelper::getBooleanValue(
            slice, StaticStrings::DataSourceDeleted, false)) {
      continue;
    }
    if (ServerState::instance()->isDBServer() &&
        arangodb::basics::VelocyPackHelper::getStringView(
            slice, StaticStrings::DataSourceType, {}) !=
            arangodb::iresearch::StaticStrings::ViewArangoSearchType) {
      continue;
    }
    result.add(slice);
  }

  result.close();

  return TRI_ERROR_NO_ERROR;
}

std::string RocksDBEngine::versionFilename(TRI_voc_tick_t id) const {
  return _basePath + TRI_DIR_SEPARATOR_CHAR + "VERSION-" + std::to_string(id);
}

void RocksDBEngine::cleanupReplicationContexts() {
  if (_replicationManager != nullptr) {
    _replicationManager->dropAll();
  }
}

VPackBuilder RocksDBEngine::getReplicationApplierConfiguration(
    TRI_vocbase_t& vocbase, ErrorCode& status) {
  RocksDBKey key;

  key.constructReplicationApplierConfig(vocbase.id());

  return getReplicationApplierConfiguration(key, status);
}

VPackBuilder RocksDBEngine::getReplicationApplierConfiguration(
    ErrorCode& status) {
  RocksDBKey key;
  key.constructReplicationApplierConfig(databaseIdForGlobalApplier);
  return getReplicationApplierConfiguration(key, status);
}

VPackBuilder RocksDBEngine::getReplicationApplierConfiguration(
    RocksDBKey const& key, ErrorCode& status) {
  rocksdb::PinnableSlice value;

  auto opts = rocksdb::ReadOptions();
  auto s = _db->Get(opts,
                    RocksDBColumnFamilyManager::get(
                        RocksDBColumnFamilyManager::Family::Definitions),
                    key.string(), &value);
  if (!s.ok()) {
    status = TRI_ERROR_FILE_NOT_FOUND;
    return arangodb::velocypack::Builder();
  }

  status = TRI_ERROR_NO_ERROR;
  VPackBuilder builder;
  builder.add(RocksDBValue::data(value));
  return builder;
}

ErrorCode RocksDBEngine::removeReplicationApplierConfiguration(
    TRI_vocbase_t& vocbase) {
  RocksDBKey key;

  key.constructReplicationApplierConfig(vocbase.id());

  return removeReplicationApplierConfiguration(key);
}

ErrorCode RocksDBEngine::removeReplicationApplierConfiguration() {
  RocksDBKey key;
  key.constructReplicationApplierConfig(databaseIdForGlobalApplier);
  return removeReplicationApplierConfiguration(key);
}

ErrorCode RocksDBEngine::removeReplicationApplierConfiguration(
    RocksDBKey const& key) {
  auto status = rocksutils::convertStatus(
      _db->Delete(rocksdb::WriteOptions(),
                  RocksDBColumnFamilyManager::get(
                      RocksDBColumnFamilyManager::Family::Definitions),
                  key.string()));
  if (!status.ok()) {
    return status.errorNumber();
  }

  return TRI_ERROR_NO_ERROR;
}

ErrorCode RocksDBEngine::saveReplicationApplierConfiguration(
    TRI_vocbase_t& vocbase, velocypack::Slice slice, bool doSync) {
  RocksDBKey key;

  key.constructReplicationApplierConfig(vocbase.id());

  return saveReplicationApplierConfiguration(key, slice, doSync);
}

ErrorCode RocksDBEngine::saveReplicationApplierConfiguration(
    velocypack::Slice slice, bool doSync) {
  RocksDBKey key;
  key.constructReplicationApplierConfig(databaseIdForGlobalApplier);
  return saveReplicationApplierConfiguration(key, slice, doSync);
}

ErrorCode RocksDBEngine::saveReplicationApplierConfiguration(
    RocksDBKey const& key, velocypack::Slice slice, bool doSync) {
  auto value = RocksDBValue::ReplicationApplierConfig(slice);

  auto status = rocksutils::convertStatus(
      _db->Put(rocksdb::WriteOptions(),
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Definitions),
               key.string(), value.string()));
  if (!status.ok()) {
    return status.errorNumber();
  }

  return TRI_ERROR_NO_ERROR;
}

// database, collection and index management
// -----------------------------------------

std::unique_ptr<TRI_vocbase_t> RocksDBEngine::openDatabase(
    arangodb::CreateDatabaseInfo&& info, bool isUpgrade) {
  return openExistingDatabase(std::move(info), true, isUpgrade);
}

Result RocksDBEngine::writeCreateDatabaseMarker(TRI_voc_tick_t id,
                                                velocypack::Slice slice) {
  return writeDatabaseMarker(id, slice, RocksDBLogValue::DatabaseCreate(id));
}

Result RocksDBEngine::writeDatabaseMarker(TRI_voc_tick_t id,
                                          velocypack::Slice slice,
                                          RocksDBLogValue&& logValue) {
  RocksDBKey key;
  key.constructDatabase(id);
  auto value = RocksDBValue::Database(slice);
  rocksdb::WriteOptions wo;

  // Write marker + key into RocksDB inside one batch
  rocksdb::WriteBatch batch;
  batch.PutLogData(logValue.slice());
  batch.Put(RocksDBColumnFamilyManager::get(
                RocksDBColumnFamilyManager::Family::Definitions),
            key.string(), value.string());
  rocksdb::Status res = _db->GetRootDB()->Write(wo, &batch);
  return rocksutils::convertStatus(res);
}

Result RocksDBEngine::writeCreateCollectionMarker(TRI_voc_tick_t databaseId,
                                                  DataSourceId cid,
                                                  velocypack::Slice slice,
                                                  RocksDBLogValue&& logValue) {
  rocksdb::DB* db = _db->GetRootDB();

  RocksDBKey key;
  key.constructCollection(databaseId, cid);
  auto value = RocksDBValue::Collection(slice);

  rocksdb::WriteOptions wo;
  // Write marker + key into RocksDB inside one batch
  rocksdb::WriteBatch batch;
  if (logValue.slice().size() > 0) {
    batch.PutLogData(logValue.slice());
  }
  batch.Put(RocksDBColumnFamilyManager::get(
                RocksDBColumnFamilyManager::Family::Definitions),
            key.string(), value.string());
  rocksdb::Status res = db->Write(wo, &batch);

  return rocksutils::convertStatus(res);
}

Result RocksDBEngine::prepareDropDatabase(TRI_vocbase_t& vocbase) {
  VPackBuilder builder;

  builder.openObject();
  builder.add("id", velocypack::Value(std::to_string(vocbase.id())));
  builder.add("name", velocypack::Value(vocbase.name()));
  builder.add("deleted", VPackValue(true));
  builder.close();

  auto log = RocksDBLogValue::DatabaseDrop(vocbase.id());
  return writeDatabaseMarker(vocbase.id(), builder.slice(), std::move(log));
}

Result RocksDBEngine::dropDatabase(TRI_vocbase_t& database) {
  replicationManager()->drop(database);
  dumpManager()->dropDatabase(database);

  return dropDatabase(database.id());
}

// current recovery state
RecoveryState RocksDBEngine::recoveryState() noexcept {
  return server().getFeature<RocksDBRecoveryManager>().recoveryState();
}

// current recovery tick
TRI_voc_tick_t RocksDBEngine::recoveryTick() noexcept {
  return server().getFeature<RocksDBRecoveryManager>().recoverySequenceNumber();
}

void RocksDBEngine::scheduleTreeRebuild(TRI_voc_tick_t database,
                                        std::string const& collection) {
  std::lock_guard locker{_rebuildCollectionsLock};
  _rebuildCollections.emplace(std::make_pair(database, collection),
                              /*started*/ false);
}

void RocksDBEngine::processTreeRebuilds() {
  Scheduler* scheduler = arangodb::SchedulerFeature::SCHEDULER;
  if (scheduler == nullptr) {
    return;
  }

  uint64_t maxParallelRebuilds = 2;
  uint64_t iterations = 0;
  while (++iterations <= maxParallelRebuilds) {
    if (server().isStopping()) {
      // don't fire off more tree rebuilds while we are shutting down
      return;
    }

    std::pair<TRI_voc_tick_t, std::string> candidate{};

    {
      std::lock_guard locker{_rebuildCollectionsLock};
      if (_rebuildCollections.empty() ||
          _runningRebuilds >= maxParallelRebuilds) {
        // nothing to do, or too much to do
        return;
      }

      for (auto& it : _rebuildCollections) {
        if (!it.second) {
          // set to started
          it.second = true;
          candidate = it.first;
          ++_runningRebuilds;
          break;
        }
      }
    }

    if (candidate.first == 0 || candidate.second.empty()) {
      return;
    }

    if (server().isStopping()) {
      return;
    }

    scheduler->queue(arangodb::RequestLane::CLIENT_SLOW, [this, candidate]() {
      if (!server().isStopping()) {
        VocbasePtr vocbase;
        try {
          auto& df = server().getFeature<DatabaseFeature>();
          vocbase = df.useDatabase(candidate.first);
          if (vocbase != nullptr) {
            auto collection = vocbase->lookupCollectionByUuid(candidate.second);
            if (collection != nullptr && !collection->deleted()) {
              LOG_TOPIC("b96bc", INFO, Logger::ENGINES)
                  << "starting background rebuild of revision tree for "
                     "collection "
                  << candidate.first << "/" << collection->name();
              Result res =
                  static_cast<RocksDBCollection*>(collection->getPhysical())
                      ->rebuildRevisionTree()
                      .waitAndGet();
              if (res.ok()) {
                ++_metricsTreeRebuildsSuccess;
                LOG_TOPIC("2f997", INFO, Logger::ENGINES)
                    << "successfully rebuilt revision tree for collection "
                    << candidate.first << "/" << collection->name();
              } else {
                ++_metricsTreeRebuildsFailure;
                if (res.is(TRI_ERROR_LOCK_TIMEOUT)) {
                  LOG_TOPIC("bce3a", WARN, Logger::ENGINES)
                      << "failure during revision tree rebuilding for "
                         "collection "
                      << candidate.first << "/" << collection->name() << ": "
                      << res.errorMessage();
                } else {
                  LOG_TOPIC("a1fc2", ERR, Logger::ENGINES)
                      << "failure during revision tree rebuilding for "
                         "collection "
                      << candidate.first << "/" << collection->name() << ": "
                      << res.errorMessage();
                }
                {
                  // mark as to-be-done again
                  std::lock_guard locker{_rebuildCollectionsLock};
                  auto it = _rebuildCollections.find(candidate);
                  if (it != _rebuildCollections.end()) {
                    (*it).second = false;
                  }
                }
                // rethrow exception
                THROW_ARANGO_EXCEPTION(res);
              }
            }
          }

          // tree rebuilding finished successfully. now remove from the list
          // to-be-rebuilt candidates
          std::lock_guard locker{_rebuildCollectionsLock};
          _rebuildCollections.erase(candidate);

        } catch (std::exception const& ex) {
          LOG_TOPIC("13afc", WARN, Logger::ENGINES)
              << "caught exception during tree rebuilding: " << ex.what();
        } catch (...) {
          LOG_TOPIC("0bcbf", WARN, Logger::ENGINES)
              << "caught unknown exception during tree rebuilding";
        }
      }

      // always count down _runningRebuilds!
      std::lock_guard locker{_rebuildCollectionsLock};
      TRI_ASSERT(_runningRebuilds > 0);
      --_runningRebuilds;
    });
  }
}

void RocksDBEngine::compactRange(RocksDBKeyBounds bounds) {
  {
    WRITE_LOCKER(locker, _pendingCompactionsLock);
    _pendingCompactions.push_back(std::move(bounds));
  }

  // directly kick off compactions if there is enough processing
  // capacity
  processCompactions();
}

void RocksDBEngine::processCompactions() {
  Scheduler* scheduler = arangodb::SchedulerFeature::SCHEDULER;
  if (scheduler == nullptr) {
    return;
  }

  uint64_t maxIterations = _maxParallelCompactions;
  uint64_t iterations = 0;
  while (++iterations <= maxIterations) {
    if (server().isStopping()) {
      // don't fire off more compactions while we are shutting down
      return;
    }

    RocksDBKeyBounds bounds = RocksDBKeyBounds::Empty();
    {
      WRITE_LOCKER(locker, _pendingCompactionsLock);
      if (_pendingCompactions.empty() ||
          _runningCompactions >= _maxParallelCompactions) {
        // nothing to do, or too much to do
        LOG_TOPIC("d5108", TRACE, Logger::ENGINES)
            << "not scheduling compactions. pending: "
            << _pendingCompactions.size()
            << ", running: " << _runningCompactions;
        return;
      }
      rocksdb::ColumnFamilyHandle* cfh =
          _pendingCompactions.front().columnFamily();

      if (!_runningCompactionsColumnFamilies.emplace(cfh).second) {
        // a compaction is already running for the same column family.
        // we don't want to schedule parallel compactions for the same column
        // family because they can lead to shutdown issues (this is an issue of
        // RocksDB).
        LOG_TOPIC("ac8b9", TRACE, Logger::ENGINES)
            << "not scheduling compactions. already have a compaction running "
               "for column family '"
            << cfh->GetName() << "', running: " << _runningCompactions;
        return;
      }

      // found something to do, now steal the item from the queue
      bounds = std::move(_pendingCompactions.front());
      _pendingCompactions.pop_front();

      if (server().isStopping()) {
        // if we are stopping, it is ok to not process but lose any pending
        // compactions
        return;
      }

      // set it to running already, so that concurrent callers of this method
      // will not kick off additional jobs
      ++_runningCompactions;

      LOG_TOPIC("6ea1b", TRACE, Logger::ENGINES)
          << "scheduling compaction in column family '" << cfh->GetName()
          << "' for execution";
    }

    scheduler->queue(arangodb::RequestLane::CLIENT_SLOW, [this, bounds]() {
      if (server().isStopping()) {
        LOG_TOPIC("3d619", TRACE, Logger::ENGINES)
            << "aborting pending compaction due to server shutdown";
      } else {
        LOG_TOPIC("9485b", TRACE, Logger::ENGINES)
            << "executing compaction for range " << bounds;
        double start = TRI_microtime();
        try {
          rocksdb::CompactRangeOptions opts;
          opts.exclusive_manual_compaction = false;
          opts.allow_write_stall = true;
          opts.canceled = &::cancelCompactions;
          rocksdb::Slice b = bounds.start(), e = bounds.end();
          _db->CompactRange(opts, bounds.columnFamily(), &b, &e);
        } catch (std::exception const& ex) {
          LOG_TOPIC("a4c42", WARN, Logger::ENGINES)
              << "compaction for range " << bounds
              << " failed with error: " << ex.what();
        } catch (...) {
          // whatever happens, we need to count down _runningCompactions in
          // all cases
        }

        LOG_TOPIC("79591", TRACE, Logger::ENGINES)
            << "finished compaction for range " << bounds
            << ", took: " << Logger::FIXED(TRI_microtime() - start);
      }
      // always count down _runningCompactions!
      WRITE_LOCKER(locker, _pendingCompactionsLock);

      TRI_ASSERT(_runningCompactionsColumnFamilies.size() ==
                 _runningCompactions);

      TRI_ASSERT(_runningCompactions > 0);
      --_runningCompactions;

      TRI_ASSERT(
          _runningCompactionsColumnFamilies.contains(bounds.columnFamily()));
      _runningCompactionsColumnFamilies.erase(bounds.columnFamily());
    });
  }
}

void RocksDBEngine::createCollection(TRI_vocbase_t& vocbase,
                                     LogicalCollection const& collection) {
  DataSourceId const cid = collection.id();
  TRI_ASSERT(cid.isSet());

  auto builder = collection.toVelocyPackIgnore(
      {"path", "statusString"},
      LogicalDataSource::Serialization::PersistenceWithInProgress);
  TRI_UpdateTickServer(cid.id());

  Result res = writeCreateCollectionMarker(
      vocbase.id(), cid, builder.slice(),
      RocksDBLogValue::CollectionCreate(vocbase.id(), cid));

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}

void RocksDBEngine::prepareDropCollection(TRI_vocbase_t& /*vocbase*/,
                                          LogicalCollection& coll) {
  replicationManager()->drop(coll);
}

Result RocksDBEngine::dropCollection(TRI_vocbase_t& vocbase,
                                     LogicalCollection& coll) {
  auto* rcoll = static_cast<RocksDBMetaCollection*>(coll.getPhysical());
  bool const prefixSameAsStart = true;
  bool const useRangeDelete = rcoll->meta().numberDocuments() >= 32 * 1024;

  auto resLock = rcoll->lockWrite().waitAndGet();  // technically not necessary
  if (resLock != TRI_ERROR_NO_ERROR) {
    return resLock;
  }

  rocksdb::DB* db = _db->GetRootDB();

  // If we get here the collection is safe to drop.
  //
  // This uses the following workflow:
  // 1. Persist the drop.
  //   * if this fails the collection will remain!
  //   * if this succeeds the collection is gone from user point
  // 2. Drop all Documents
  //   * If this fails we give up => We have data-garbage in RocksDB,
  //   Collection is gone.
  // 3. Drop all Indexes
  //   * If this fails we give up => We have data-garbage in RocksDB,
  //   Collection is gone.
  // 4. If all succeeds we do not have data-garbage, all is gone.
  //
  // (NOTE: The above fails can only occur on full HDD or Machine dying. No
  // write conflicts possible)

  TRI_ASSERT(coll.deleted());

  // Prepare collection remove batch
  rocksdb::WriteBatch batch;
  RocksDBLogValue logValue =
      RocksDBLogValue::CollectionDrop(vocbase.id(), coll.id(), coll.guid());
  batch.PutLogData(logValue.slice());

  RocksDBKey key;
  key.constructCollection(vocbase.id(), coll.id());
  batch.Delete(RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Definitions),
               key.string());

  rocksdb::WriteOptions wo;
  rocksdb::Status s = db->Write(wo, &batch);

  // TODO FAILURE Simulate !res.ok()
  if (!s.ok()) {
    // Persisting the drop failed. Do NOT drop collection.
    return rocksutils::convertStatus(s);
  }

  // Now Collection is gone.
  // Cleanup data-mess

  // Unregister collection metadata
  Result res = RocksDBMetadata::deleteCollectionMeta(db, rcoll->objectId());
  if (res.fail()) {
    LOG_TOPIC("2c890", ERR, Logger::ENGINES)
        << "error removing collection meta-data: "
        << res.errorMessage();  // continue regardless
  }

  // remove from map
  removeCollectionMapping(rcoll->objectId());

  // delete indexes, RocksDBIndex::drop() has its own check
  auto indexes = rcoll->getAllIndexes();
  TRI_ASSERT(!indexes.empty());

  for (auto const& idx : indexes) {
    auto* rIdx = basics::downCast<RocksDBIndex>(idx.get());
    res = RocksDBMetadata::deleteIndexEstimate(db, rIdx->objectId());
    if (res.fail()) {
      LOG_TOPIC("f2d51", WARN, Logger::ENGINES)
          << "could not delete index estimate: " << res.errorMessage();
    }

    auto dropRes = idx->drop();

    if (dropRes.fail()) {
      // We try to remove all indexed values.
      // If it does not work they cannot be accessed any more and leaked.
      // User View remains consistent.
      LOG_TOPIC("97176", ERR, Logger::ENGINES)
          << "unable to drop index: " << dropRes.errorMessage();
    }
  }

  // delete documents
  RocksDBKeyBounds bounds =
      RocksDBKeyBounds::CollectionDocuments(rcoll->objectId());
  auto result = rocksutils::removeLargeRange(db, bounds, prefixSameAsStart,
                                             useRangeDelete);

  if (result.fail()) {
    // We try to remove all documents.
    // If it does not work they cannot be accessed any more and leaked.
    // User View remains consistent.
    return {};
  }

  // run compaction for data only if collection contained a considerable
  // amount of documents. otherwise don't run compaction, because it will
  // slow things down a lot, especially during tests that create/drop LOTS
  // of collections
  if (useRangeDelete) {
    rcoll->compact();
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // check if documents have been deleted
  size_t numDocs = rocksutils::countKeyRange(_db, bounds, nullptr, true);

  if (numDocs > 0) {
    std::string errorMsg(
        "deletion check in collection drop failed - not all documents "
        "have been deleted. remaining: ");
    errorMsg.append(std::to_string(numDocs));
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, errorMsg);
  }
#endif

  // if we get here all documents / indexes are gone.
  // We have no data garbage left.
  return {};
}

void RocksDBEngine::changeCollection(TRI_vocbase_t& vocbase,
                                     LogicalCollection const& collection) {
  auto builder = collection.toVelocyPackIgnore(
      {"path", "statusString"},
      LogicalDataSource::Serialization::PersistenceWithInProgress);
  Result res = writeCreateCollectionMarker(
      vocbase.id(), collection.id(), builder.slice(),
      RocksDBLogValue::CollectionChange(vocbase.id(), collection.id()));

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}

Result RocksDBEngine::renameCollection(TRI_vocbase_t& vocbase,
                                       LogicalCollection const& collection,
                                       std::string const& oldName) {
  auto builder = collection.toVelocyPackIgnore(
      {"path", "statusString"},
      LogicalDataSource::Serialization::PersistenceWithInProgress);
  Result res = writeCreateCollectionMarker(
      vocbase.id(), collection.id(), builder.slice(),
      RocksDBLogValue::CollectionRename(vocbase.id(), collection.id(),
                                        oldName));

  return res;
}

Result RocksDBEngine::createView(TRI_vocbase_t& vocbase, DataSourceId id,
                                 LogicalView const& view) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_TOPIC("0bad8", DEBUG, Logger::ENGINES) << "RocksDBEngine::createView";
#endif
  rocksdb::WriteBatch batch;
  rocksdb::WriteOptions wo;

  RocksDBKey key;
  key.constructView(vocbase.id(), id);
  RocksDBLogValue logValue = RocksDBLogValue::ViewCreate(vocbase.id(), id);

  VPackBuilder props;

  props.openObject();
  auto result = view.properties(
      props, LogicalDataSource::Serialization::PersistenceWithInProgress);
  if (result.fail()) {
    LOG_TOPIC_IF("234ae", WARN, Logger::VIEWS, !result.ok())
        << "could not serialize view " << id << ": " << result;
    return result;
  }
  props.close();

  RocksDBValue const value = RocksDBValue::View(props.slice());

  // Write marker + key into RocksDB inside one batch
  batch.PutLogData(logValue.slice());
  batch.Put(RocksDBColumnFamilyManager::get(
                RocksDBColumnFamilyManager::Family::Definitions),
            key.string(), value.string());

  auto res = _db->Write(wo, &batch);

  LOG_TOPIC_IF("cac6a", TRACE, Logger::VIEWS, !res.ok())
      << "could not create view: " << res.ToString();

  return rocksutils::convertStatus(res);
}

Result RocksDBEngine::dropView(TRI_vocbase_t const& vocbase,
                               LogicalView const& view) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_TOPIC("fa6e5", DEBUG, Logger::ENGINES) << "RocksDBEngine::dropView";
#endif
  auto logValue =
      RocksDBLogValue::ViewDrop(vocbase.id(), view.id(), view.guid());

  RocksDBKey key;
  key.constructView(vocbase.id(), view.id());

  rocksdb::WriteBatch batch;
  batch.PutLogData(logValue.slice());
  batch.Delete(RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Definitions),
               key.string());

  rocksdb::WriteOptions wo;
  auto res = _db->GetRootDB()->Write(wo, &batch);
  LOG_TOPIC_IF("fcd22", TRACE, Logger::VIEWS, !res.ok())
      << "could not create view: " << res.ToString();
  return rocksutils::convertStatus(res);
}

Result RocksDBEngine::changeView(LogicalView const& view,
                                 velocypack::Slice update) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_TOPIC("405da", DEBUG, Logger::ENGINES) << "RocksDBEngine::changeView";
#endif
  if (inRecovery()) {
    // nothing to do
    return {};
  }
  auto& vocbase = view.vocbase();

  RocksDBKey key;
  key.constructView(vocbase.id(), view.id());

  RocksDBLogValue log = RocksDBLogValue::ViewChange(vocbase.id(), view.id());
  RocksDBValue const value = RocksDBValue::View(update);

  rocksdb::WriteBatch batch;
  rocksdb::WriteOptions wo;  // TODO: check which options would make sense
  rocksdb::Status s;

  s = batch.PutLogData(log.slice());

  if (!s.ok()) {
    LOG_TOPIC("6d6a4", TRACE, Logger::VIEWS)
        << "failed to write change view marker " << s.ToString();
    return rocksutils::convertStatus(s);
  }

  s = batch.Put(RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Definitions),
                key.string(), value.string());

  if (!s.ok()) {
    LOG_TOPIC("ebb58", TRACE, Logger::VIEWS)
        << "failed to write change view marker " << s.ToString();
    return rocksutils::convertStatus(s);
  }
  auto res = _db->Write(wo, &batch);
  LOG_TOPIC_IF("6ee8a", TRACE, Logger::VIEWS, !res.ok())
      << "could not change view: " << res.ToString();
  return rocksutils::convertStatus(res);
}

Result RocksDBEngine::compactAll(bool changeLevel,
                                 bool compactBottomMostLevel) {
  return rocksutils::compactAll(_db->GetRootDB(), changeLevel,
                                compactBottomMostLevel, &::cancelCompactions);
}

/// @brief Add engine-specific optimizer rules
void RocksDBEngine::addOptimizerRules(aql::OptimizerRulesFeature& feature) {
  RocksDBOptimizerRules::registerResources(feature);
}

#ifdef USE_V8
/// @brief Add engine-specific V8 functions
void RocksDBEngine::addV8Functions() {
  // there are no specific V8 functions here
  RocksDBV8Functions::registerResources(*this);
}
#endif

/// @brief Add engine-specific REST handlers
void RocksDBEngine::addRestHandlers(rest::RestHandlerFactory& handlerFactory) {
  RocksDBRestHandlers::registerResources(&handlerFactory);
}

void RocksDBEngine::addCollectionMapping(uint64_t objectId, TRI_voc_tick_t did,
                                         DataSourceId cid) {
  if (objectId != 0) {
    WRITE_LOCKER(guard, _mapLock);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    auto it = _collectionMap.find(objectId);
    if (it != _collectionMap.end()) {
      if (it->second.first != did || it->second.second != cid) {
        LOG_TOPIC("80e81", ERR, Logger::FIXME)
            << "trying to add objectId: " << objectId << ", did: " << did
            << ", cid: " << cid.id()
            << ", found in map: did: " << it->second.first
            << ", cid: " << it->second.second.id() << ", map contains "
            << _collectionMap.size() << " entries";
        for (auto const& it : _collectionMap) {
          LOG_TOPIC("77de9", ERR, Logger::FIXME)
              << "- objectId: " << it.first << " => (did: " << it.second.first
              << ", cid: " << it.second.second.id() << ")";
        }
      }
      TRI_ASSERT(it->second.first == did);
      TRI_ASSERT(it->second.second == cid);
    }
#endif
    _collectionMap[objectId] = std::make_pair(did, cid);
  }
}

void RocksDBEngine::removeCollectionMapping(uint64_t objectId) {
  WRITE_LOCKER(guard, _mapLock);
  _collectionMap.erase(objectId);
}

std::vector<std::tuple<uint64_t, TRI_voc_tick_t, DataSourceId>>
RocksDBEngine::collectionMappings() const {
  std::vector<std::tuple<uint64_t, TRI_voc_tick_t, DataSourceId>> res;

  READ_LOCKER(guard, _mapLock);
  res.reserve(_collectionMap.size());
  for (auto const& it : _collectionMap) {
    res.emplace_back(it.first, it.second.first, it.second.second);
  }
  return res;
}

void RocksDBEngine::addIndexMapping(uint64_t objectId, TRI_voc_tick_t did,
                                    DataSourceId cid, IndexId iid) {
  if (objectId != 0) {
    WRITE_LOCKER(guard, _mapLock);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    auto it = _indexMap.find(objectId);
    if (it != _indexMap.end()) {
      TRI_ASSERT(std::get<0>(it->second) == did);
      TRI_ASSERT(std::get<1>(it->second) == cid);
      TRI_ASSERT(std::get<2>(it->second) == iid);
    }
#endif
    _indexMap[objectId] = std::make_tuple(did, cid, iid);
  }
}

void RocksDBEngine::removeIndexMapping(uint64_t objectId) {
  if (objectId != 0) {
    WRITE_LOCKER(guard, _mapLock);
    _indexMap.erase(objectId);
  }
}

RocksDBEngine::CollectionPair RocksDBEngine::mapObjectToCollection(
    uint64_t objectId) const {
  READ_LOCKER(guard, _mapLock);
  auto it = _collectionMap.find(objectId);
  if (it == _collectionMap.end()) {
    return {0, DataSourceId::none()};
  }
  return it->second;
}

RocksDBEngine::IndexTriple RocksDBEngine::mapObjectToIndex(
    uint64_t objectId) const {
  READ_LOCKER(guard, _mapLock);
  auto it = _indexMap.find(objectId);
  if (it == _indexMap.end()) {
    return RocksDBEngine::IndexTriple(0, 0, 0);
  }
  return it->second;
}

/// @brief return a list of the currently open WAL files
std::vector<std::string> RocksDBEngine::currentWalFiles() const {
  rocksdb::VectorLogPtr files;
  std::vector<std::string> names;

  auto status = _db->GetSortedWalFiles(files);
  if (!status.ok()) {
    return names;  // TODO: error here?
  }

  for (size_t current = 0; current < files.size(); current++) {
    auto f = files[current].get();
    try {
      names.push_back(f->PathName());
    } catch (...) {
      return names;
    }
  }

  return names;
}

/// @brief flushes the RocksDB WAL.
/// the optional parameter "waitForSync" is currently only used when the
/// "flushColumnFamilies" parameter is also set to true. If
/// "flushColumnFamilies" is true, all the RocksDB column family memtables are
/// flushed, and, if "waitForSync" is set, additionally synced to disk. The
/// only call site that uses "flushColumnFamilies" currently is hot backup.
/// The function parameter name are a remainder from MMFiles times, when they
/// made more sense. This can be refactored at any point, so that flushing
/// column families becomes a separate API.
Result RocksDBEngine::flushWal(bool waitForSync, bool flushColumnFamilies) {
  Result res;

  if (_syncThread) {
    // _syncThread may be a nullptr, in case automatic syncing is turned off
    res = _syncThread->syncWal();
  } else {
    // no syncThread...
    res = RocksDBSyncThread::sync(_db->GetBaseDB());
  }

  if (res.ok() && flushColumnFamilies) {
    rocksdb::FlushOptions flushOptions;
    flushOptions.wait = waitForSync;

    for (auto cf : RocksDBColumnFamilyManager::allHandles()) {
      rocksdb::Status status = _db->GetBaseDB()->Flush(flushOptions, cf);
      if (!status.ok()) {
        res.reset(rocksutils::convertStatus(status));
        break;
      }
    }
  }

  return res;
}

void RocksDBEngine::waitForEstimatorSync() {
  // release all unused ticks from flush feature
  server().getFeature<FlushFeature>().releaseUnusedTicks();

  // force-flush
  _settingsManager->sync(/*force*/ true);
}

Result RocksDBEngine::registerRecoveryHelper(
    std::shared_ptr<RocksDBRecoveryHelper> helper) {
  try {
    _recoveryHelpers.emplace_back(std::move(helper));
  } catch (std::bad_alloc const&) {
    return {TRI_ERROR_OUT_OF_MEMORY};
  }

  return {};
}

std::vector<std::shared_ptr<RocksDBRecoveryHelper>> const&
RocksDBEngine::recoveryHelpers() {
  return _recoveryHelpers;
}

void RocksDBEngine::determineWalFilesInitial() {
  WRITE_LOCKER(lock, _walFileLock);
  // Retrieve the sorted list of all wal files with earliest file first
  rocksdb::VectorLogPtr files;
  auto status = _db->GetSortedWalFiles(files);
  if (!status.ok()) {
    LOG_TOPIC("078ee", WARN, Logger::ENGINES)
        << "could not get WAL files: " << status.ToString();
    return;
  }

  size_t liveFiles = 0;
  size_t archivedFiles = 0;
  uint64_t liveFilesSize = 0;
  uint64_t archivedFilesSize = 0;
  for (size_t current = 0; current < files.size(); current++) {
    auto const& f = files[current].get();

    if (f->Type() == rocksdb::WalFileType::kArchivedLogFile) {
      ++archivedFiles;
      archivedFilesSize += f->SizeFileBytes();
    } else if (f->Type() == rocksdb::WalFileType::kAliveLogFile) {
      ++liveFiles;
      liveFilesSize += f->SizeFileBytes();
    }
  }
  _metricsWalSequenceLowerBound.store(_settingsManager->earliestSeqNeeded(),
                                      std::memory_order_relaxed);
  _metricsLiveWalFiles.store(liveFiles, std::memory_order_relaxed);
  _metricsArchivedWalFiles.store(archivedFiles, std::memory_order_relaxed);
  _metricsLiveWalFilesSize.store(liveFilesSize, std::memory_order_relaxed);
  _metricsArchivedWalFilesSize.store(archivedFilesSize,
                                     std::memory_order_relaxed);
}

void RocksDBEngine::determinePrunableWalFiles(TRI_voc_tick_t minTickExternal) {
  WRITE_LOCKER(lock, _walFileLock);
  TRI_voc_tick_t minTickToKeep =
      std::min(_useReleasedTick ? _releasedTick
                                : std::numeric_limits<TRI_voc_tick_t>::max(),
               minTickExternal);

  uint64_t minLogNumberToKeep = 0;
  std::string v;
  if (_db->GetProperty(rocksdb::DB::Properties::kMinLogNumberToKeep, &v)) {
    minLogNumberToKeep = static_cast<uint64_t>(basics::StringUtils::int64(v));
  }

  LOG_TOPIC("4673c", DEBUG, Logger::ENGINES)
      << "determining prunable WAL files, minTickToKeep: " << minTickToKeep
      << ", minTickExternal: " << minTickExternal
      << ", releasedTick: " << _releasedTick
      << ", minLogNumberToKeep: " << minLogNumberToKeep;

  // Retrieve the sorted list of all wal files with earliest file first
  rocksdb::VectorLogPtr files;
  auto status = _db->GetSortedWalFiles(files);
  if (!status.ok()) {
    LOG_TOPIC("078ef", WARN, Logger::ENGINES)
        << "could not get WAL files: " << status.ToString();
    return;
  }

  // number of live WAL files
  size_t liveFiles = 0;
  // number of archived WAL files
  size_t archivedFiles = 0;
  // cumulated size of live WAL files
  uint64_t liveFilesSize = 0;
  // cumulated size of archived WAL files
  uint64_t archivedFilesSize = 0;

  for (size_t current = 0; current < files.size(); current++) {
    auto const& f = files[current].get();

    if (f->Type() == rocksdb::WalFileType::kAliveLogFile) {
      ++liveFiles;
      liveFilesSize += f->SizeFileBytes();
      LOG_TOPIC("dc472", TRACE, Logger::ENGINES)
          << "live WAL file #" << current << "/" << files.size()
          << ", filename: '" << f->PathName()
          << "', start sequence: " << f->StartSequence();
      continue;
    }

    if (f->Type() != rocksdb::WalFileType::kArchivedLogFile) {
      // we are mostly interested in files of the archive
      continue;
    }

    ++archivedFiles;
    archivedFilesSize += f->SizeFileBytes();

    // check if there is another WAL file coming after the currently-looked-at
    // There should be at least one live WAL file after it, however, let's be
    // paranoid and do a proper check. If there is at least one WAL file
    // following, we need to take its start tick into account as well, because
    // the following file's start tick can be assumed to be the end tick of
    // the current file!
    bool eligibleStep1 = false;
    bool eligibleStep2 = false;
    if (f->StartSequence() < minTickToKeep && current < files.size() - 1) {
      eligibleStep1 = true;
      auto const& n = files[current + 1].get();
      if (n->StartSequence() < minTickToKeep) {
        // this file will be removed because it does not contain any data we
        // still need
        eligibleStep2 = true;

        double stamp = TRI_microtime() + _pruneWaitTime;
        auto const [it, emplaced] =
            _prunableWalFiles.try_emplace(f->PathName(), stamp);

        if (emplaced) {
          LOG_TOPIC("9f7a4", DEBUG, Logger::ENGINES)
              << "RocksDB WAL file '" << f->PathName()
              << "' with start sequence " << f->StartSequence()
              << ", expire stamp " << stamp
              << " added to prunable list because it is not needed anymore";
          TRI_ASSERT(it != _prunableWalFiles.end());
        } else {
          LOG_TOPIC("d2c9e", TRACE, Logger::ENGINES)
              << "unable to add WAL file #" << current << "/" << files.size()
              << ", filename: '" << f->PathName()
              << "', start sequence: " << f->StartSequence()
              << " to list of prunable WAL files. file already present in "
                 "list "
                 "with expire stamp "
              << it->second;
        }
      }
    }

    LOG_TOPIC("ec350", TRACE, Logger::ENGINES)
        << "inspected WAL file #" << current << "/" << files.size()
        << ", filename: '" << f->PathName()
        << "', start sequence: " << f->StartSequence()
        << ", eligible step1: " << eligibleStep1
        << ", step2: " << eligibleStep2;
  }

  LOG_TOPIC("01e20", DEBUG, Logger::ENGINES)
      << "found " << files.size() << " WAL file(s), with " << liveFiles
      << " live file(s) and " << archivedFiles << " file(s) in the archive, "
      << "number of prunable files: " << _prunableWalFiles.size()
      << ", live file size: " << liveFilesSize
      << ", archived file size: " << archivedFilesSize;

  if (_maxWalArchiveSizeLimit > 0 &&
      archivedFilesSize > _maxWalArchiveSizeLimit) {
    // size of the archive is restricted, and we overflowed the limit.

    // print current archive size
    LOG_TOPIC("8d71b", TRACE, Logger::ENGINES)
        << "total size of the RocksDB WAL file archive: " << archivedFilesSize
        << ", limit: " << _maxWalArchiveSizeLimit;

    // we got more archived files than configured. time for purging some
    // files!
    for (size_t current = 0; current < files.size(); current++) {
      auto const& f = files[current].get();

      if (f->Type() != rocksdb::WalFileType::kArchivedLogFile) {
        continue;
      }

      // force pruning
      bool doPrint = false;
      auto [it, emplaced] = _prunableWalFiles.try_emplace(f->PathName(), -1.0);
      if (emplaced) {
        doPrint = true;
      } else {
        // file already in list. now set its expiration time to the past
        // so we are sure it will get deleted

        // using an expiration time of -1.0 indicates the file is subject to
        // deletion because the archive outgrew the maximum allowed size
        if ((*it).second > 0.0) {
          doPrint = true;
        }
        (*it).second = -1.0;
      }

      if (doPrint) {
        TRI_ASSERT(archivedFilesSize > _maxWalArchiveSizeLimit);

        // never change this id without adjusting wal-archive-size-limit tests
        // in tests/js/client/server-parameters
        LOG_TOPIC("d9793", WARN, Logger::ENGINES)
            << "forcing removal of RocksDB WAL file '" << f->PathName()
            << "' with start sequence " << f->StartSequence()
            << " because of overflowing archive. configured maximum archive "
               "size is "
            << _maxWalArchiveSizeLimit
            << ", actual archive size is: " << archivedFilesSize
            << ". if these warnings persist, try to increase the value of "
            << "the startup option `--rocksdb.wal-archive-size-limit`";
      }

      TRI_ASSERT(archivedFilesSize >= f->SizeFileBytes());
      archivedFilesSize -= f->SizeFileBytes();

      if (archivedFilesSize <= _maxWalArchiveSizeLimit) {
        // got enough files to remove
        break;
      }
    }
  }

  _metricsWalSequenceLowerBound.store(_settingsManager->earliestSeqNeeded(),
                                      std::memory_order_relaxed);
  _metricsLiveWalFiles.store(liveFiles, std::memory_order_relaxed);
  _metricsArchivedWalFiles.store(archivedFiles, std::memory_order_relaxed);
  _metricsLiveWalFilesSize.store(liveFilesSize, std::memory_order_relaxed);
  _metricsArchivedWalFilesSize.store(archivedFilesSize,
                                     std::memory_order_relaxed);
  _metricsPrunableWalFiles.store(_prunableWalFiles.size(),
                                 std::memory_order_relaxed);
  _metricsWalPruningActive.store(1, std::memory_order_relaxed);
}

RocksDBFilePurgePreventer RocksDBEngine::disallowPurging() noexcept {
  return RocksDBFilePurgePreventer(this);
}

RocksDBFilePurgeEnabler RocksDBEngine::startPurging() noexcept {
  return RocksDBFilePurgeEnabler(this);
}

void RocksDBEngine::pruneWalFiles() {
  // this struct makes sure that no other threads enter WAL tailing while we
  // are in here. If there are already other threads in WAL tailing while we
  // get here, we go on and only remove the WAL files that are really safe
  // to remove
  RocksDBFilePurgeEnabler purgeEnabler(startPurging());

  WRITE_LOCKER(lock, _walFileLock);

  // used for logging later
  size_t const initialSize = _prunableWalFiles.size();

  // go through the map of WAL files that we have already and check if they
  // are "expired"
  for (auto it = _prunableWalFiles.begin(); it != _prunableWalFiles.end();
       /* no hoisting */) {
    // check if WAL file is expired
    auto deleteFile = purgeEnabler.canPurge();
    LOG_TOPIC("e7674", TRACE, Logger::ENGINES)
        << "pruneWalFiles checking file '" << (*it).first
        << "', canPurge: " << deleteFile;

    if (deleteFile) {
      LOG_TOPIC("68e4a", DEBUG, Logger::ENGINES)
          << "deleting RocksDB WAL file '" << (*it).first << "'";
      rocksdb::Status s;
      if (basics::FileUtils::exists(basics::FileUtils::buildFilename(
              _dbOptions.wal_dir, (*it).first))) {
        // only attempt file deletion if the file actually exists.
        // otherwise RocksDB may complain about non-existing files and log a
        // big error message
        s = _db->DeleteFile((*it).first);
        LOG_TOPIC("5b1ae", DEBUG, Logger::ENGINES)
            << "calling RocksDB DeleteFile for WAL file '" << (*it).first
            << "'. status: " << rocksutils::convertStatus(s).errorMessage();
      } else {
        LOG_TOPIC("c2cc9", DEBUG, Logger::ENGINES)
            << "to-be-deleted RocksDB WAL file '" << (*it).first
            << "' does not exist. skipping deletion";
      }
      // apparently there is a case where a file was already deleted
      // but is still in _prunableWalFiles. In this case we get an invalid
      // argument response.
      if (s.ok() || s.IsInvalidArgument()) {
        it = _prunableWalFiles.erase(it);
        continue;
      } else {
        LOG_TOPIC("83162", WARN, Logger::ENGINES)
            << "attempt to prune RocksDB WAL file '" << (*it).first
            << "' failed with error: "
            << rocksutils::convertStatus(s).errorMessage();
      }
    }

    // cannot delete this file yet... must forward iterator to prevent an
    // endless loop
    ++it;
  }

  _metricsPrunableWalFiles.store(_prunableWalFiles.size(),
                                 std::memory_order_relaxed);

  LOG_TOPIC("82a4c", TRACE, Logger::ENGINES)
      << "prune WAL files started with " << initialSize
      << " prunable WAL files, "
      << "current number of prunable WAL files: " << _prunableWalFiles.size();
}

Result RocksDBEngine::dropReplicatedStates(TRI_voc_tick_t databaseId) {
  auto* cfDefs = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Definitions);

  auto bounds = RocksDBKeyBounds::DatabaseStates(databaseId);
  auto upper = bounds.end();
  auto readOptions = rocksdb::ReadOptions();
  readOptions.iterate_upper_bound = &upper;

  auto rv = Result();

  auto iter =
      std::unique_ptr<rocksdb::Iterator>(_db->NewIterator(readOptions, cfDefs));
  for (iter->Seek(bounds.start()); iter->Valid(); iter->Next()) {
    ADB_PROD_ASSERT(databaseId == RocksDBKey::databaseId(iter->key()));

    auto slice =
        VPackSlice(reinterpret_cast<uint8_t const*>(iter->value().data()));

    auto info = velocypack::deserialize<
        replication2::storage::rocksdb::ReplicatedStateInfo>(slice);
    auto* cfLogs = RocksDBColumnFamilyManager::get(
        RocksDBColumnFamilyManager::Family::ReplicatedLogs);

    auto methods = makeLogStorageMethods(info.stateId, info.objectId,
                                         databaseId, cfLogs, cfDefs);
    auto res = methods->drop();
    // Save the first error we encounter, but try to drop the rest.
    if (rv.ok() && res.fail()) {
      rv = res;
    }
    // TODO Should we do this here and now, or defer it, or don't do it at
    // all?
    //      To answer that, check whether and how compaction is usually done
    //      after dropping a collection or database.
    std::ignore = methods->compact();
  }

  return rv;
}

Result RocksDBEngine::dropDatabase(TRI_voc_tick_t id) {
  using namespace rocksutils;
  rocksdb::WriteOptions wo;
  rocksdb::DB* db = _db->GetRootDB();

  // remove view definitions
  Result res = rocksutils::removeLargeRange(
      db, RocksDBKeyBounds::DatabaseViews(id), true, /*rangeDel*/ false);
  if (res.fail()) {
    return res;
  }

  // remove replicated states
  res = dropReplicatedStates(id);
  if (res.fail()) {
    return res;
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  size_t numDocsLeft = 0;
#endif

  // remove collections
  auto dbBounds = RocksDBKeyBounds::DatabaseCollections(id);
  iterateBounds(_db, dbBounds, [&](rocksdb::Iterator* it) {
    RocksDBKey key(it->key());
    RocksDBValue value(RocksDBEntryType::Collection, it->value());

    uint64_t const objectId = basics::VelocyPackHelper::stringUInt64(
        value.slice(), StaticStrings::ObjectId);

    auto const cnt = RocksDBMetadata::loadCollectionCount(_db, objectId);
    uint64_t const numberDocuments = cnt._added - cnt._removed;
    bool const useRangeDelete = numberDocuments >= 32 * 1024;

    // remove indexes
    VPackSlice indexes = value.slice().get("indexes");
    if (indexes.isArray()) {
      for (VPackSlice it : VPackArrayIterator(indexes)) {
        // delete index documents
        uint64_t objectId =
            basics::VelocyPackHelper::stringUInt64(it, StaticStrings::ObjectId);
        res = RocksDBMetadata::deleteIndexEstimate(db, objectId);
        if (res.fail()) {
          return;
        }

        TRI_ASSERT(it.get(StaticStrings::IndexType).isString());
        auto type = Index::type(it.get(StaticStrings::IndexType).stringView());
        bool unique = basics::VelocyPackHelper::getBooleanValue(
            it, StaticStrings::IndexUnique, false);

        RocksDBKeyBounds bounds =
            RocksDBIndex::getBounds(type, objectId, unique);
        // edge index drop fails otherwise
        bool const prefixSameAsStart = type != Index::TRI_IDX_TYPE_EDGE_INDEX;
        res = rocksutils::removeLargeRange(db, bounds, prefixSameAsStart,
                                           useRangeDelete);
        if (res.fail()) {
          return;
        }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        // check if documents have been deleted
        numDocsLeft += rocksutils::countKeyRange(
            db, bounds, /*snapshot*/ nullptr, prefixSameAsStart);
#endif
      }
    }

    // delete documents
    RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(objectId);
    res = rocksutils::removeLargeRange(db, bounds, true, useRangeDelete);
    if (res.fail()) {
      LOG_TOPIC("6dbc6", WARN, Logger::ENGINES)
          << "error deleting collection documents: '" << res.errorMessage()
          << "'";
      return;
    }
    // delete collection meta-data
    res = RocksDBMetadata::deleteCollectionMeta(db, objectId);
    if (res.fail()) {
      LOG_TOPIC("484d0", WARN, Logger::ENGINES)
          << "error deleting collection metadata: '" << res.errorMessage()
          << "'";
      return;
    }
    // remove collection entry
    rocksdb::Status s =
        db->Delete(wo,
                   RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::Definitions),
                   value.string());
    if (!s.ok()) {
      LOG_TOPIC("64b4e", WARN, Logger::ENGINES)
          << "error deleting collection definition: " << s.ToString();
      return;
    }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // check if documents have been deleted
    numDocsLeft +=
        rocksutils::countKeyRange(db, bounds, /*snapshot*/ nullptr, true);
#endif
  });

  if (res.fail()) {
    return res;
  }

  // remove database meta-data
  RocksDBKey key;
  key.constructDatabase(id);
  rocksdb::Status s =
      db->Delete(wo,
                 RocksDBColumnFamilyManager::get(
                     RocksDBColumnFamilyManager::Family::Definitions),
                 key.string());
  if (!s.ok()) {
    LOG_TOPIC("9948c", WARN, Logger::ENGINES)
        << "error deleting database definition: " << s.ToString();
  }

  // remove VERSION file for database. it's not a problem when this fails
  // because it will simply remain there and be ignored on subsequent starts
  TRI_UnlinkFile(versionFilename(id).c_str());

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (numDocsLeft > 0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, absl::StrCat("deletion check in drop database "
                                         "failed - not all documents have been "
                                         "deleted. remaining: ",
                                         numDocsLeft));
  }
#endif

  return res;
}

bool RocksDBEngine::systemDatabaseExists() {
  velocypack::Builder builder;
  getDatabases(builder);

  for (auto const& item : velocypack::ArrayIterator(builder.slice())) {
    TRI_ASSERT(item.isObject());
    TRI_ASSERT(item.get(StaticStrings::DatabaseName).isString());
    if (item.get(StaticStrings::DatabaseName).stringView() ==
        StaticStrings::SystemDatabase) {
      return true;
    }
  }
  return false;
}

void RocksDBEngine::addSystemDatabase() {
  // create system database entry
  TRI_voc_tick_t id = TRI_NewTickServer();
  VPackBuilder builder;
  builder.openObject();
  builder.add(StaticStrings::DatabaseId, VPackValue(std::to_string(id)));
  builder.add(StaticStrings::DatabaseName,
              VPackValue(StaticStrings::SystemDatabase));
  builder.add("deleted", VPackValue(false));
  // Also store the ReplicationVersion when creating the Database
  auto& df = server().getFeature<DatabaseFeature>();
  builder.add(
      StaticStrings::ReplicationVersion,
      VPackValue(replication::versionToString(df.defaultReplicationVersion())));
  builder.close();

  RocksDBLogValue log = RocksDBLogValue::DatabaseCreate(id);
  Result res = writeDatabaseMarker(id, builder.slice(), std::move(log));
  if (res.fail()) {
    LOG_TOPIC("8c26a", FATAL, arangodb::Logger::STARTUP)
        << "unable to write database marker: " << res.errorMessage();
    FATAL_ERROR_EXIT();
  }
}

/// @brief open an existing database. internal function
std::unique_ptr<TRI_vocbase_t> RocksDBEngine::openExistingDatabase(
    CreateDatabaseInfo&& info, bool wasCleanShutdown, bool isUpgrade) {
  auto vocbase = createDatabase(std::move(info));

  LOG_TOPIC("26c21", TRACE, arangodb::Logger::ENGINES)
      << "opening views and collections metadata in database '"
      << vocbase->name() << "'";

  VPackBuilder builder;
  auto scanViews = [&](std::string_view type) {
    try {
      if (builder.isEmpty()) {
        auto r = getViews(*vocbase, builder);
        if (r != TRI_ERROR_NO_ERROR) {
          THROW_ARANGO_EXCEPTION(r);
        }
      }

      auto const slice = builder.slice();
      TRI_ASSERT(slice.isArray());

      LOG_TOPIC("48f11", TRACE, arangodb::Logger::ENGINES)
          << "processing views metadata in database '" << vocbase->name()
          << "': " << slice.toJson();

      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.get(StaticStrings::DataSourceType).stringView() != type) {
          continue;
        }

        // we found a view that is still active
        LOG_TOPIC("4dfdd", TRACE, arangodb::Logger::ENGINES)
            << "processing view metadata in database '" << vocbase->name()
            << "': " << it.toJson();

        TRI_ASSERT(!it.get("id").isNone());

        Result res;
        try {
          LogicalView::ptr view;
          res = LogicalView::instantiate(view, *vocbase, it, false);

          if (res.ok() && !view) {
            res.reset(TRI_ERROR_INTERNAL);
          }

          if (!res.ok()) {
            THROW_ARANGO_EXCEPTION(res);
          }

          TRI_ASSERT(view);

          StorageEngine::registerView(*vocbase, view);
          view->open();
        } catch (basics::Exception const& ex) {
          res.reset(ex.code(), ex.what());
        }

        if (!res.ok()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              res.errorNumber(),
              absl::StrCat("failed to instantiate view in database '",
                           vocbase->name(), "' from definition: ",
                           it.toString(), ": ", res.errorMessage()));
        }
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("584b1", ERR, arangodb::Logger::ENGINES)
          << "error while opening database '" << vocbase->name()
          << "': " << ex.what();
      throw;
    } catch (...) {
      LOG_TOPIC("593fd", ERR, arangodb::Logger::ENGINES)
          << "error while opening database '" << vocbase->name()
          << "': unknown exception";
      throw;
    }
  };

  // scan the database path for "arangosearch" views
  scanViews(iresearch::StaticStrings::ViewArangoSearchType);

  // replicated states should be loaded before their respective shards
  if (vocbase->replicationVersion() == replication::Version::TWO) {
    if (syncThread() == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_ILLEGAL_OPTION,
          "Automatic syncing must be enabled for replication "
          "version 2. Please make sure the --rocksdb.sync-interval "
          "option is set to a value greater than 0.");
    }
    try {
      loadReplicatedStates(*vocbase);
    } catch (std::exception const& ex) {
      LOG_TOPIC("554c1", ERR, arangodb::Logger::ENGINES)
          << "error while opening database: " << ex.what();
      throw;
    } catch (...) {
      LOG_TOPIC("5f33d", ERR, arangodb::Logger::ENGINES)
          << "error while opening database: unknown exception";
      throw;
    }
  }

  // scan the database path for collections
  try {
    VPackBuilder builder;
    auto res = getCollectionsAndIndexes(*vocbase, builder, wasCleanShutdown,
                                        isUpgrade);

    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }

    VPackSlice slice = builder.slice();
    TRI_ASSERT(slice.isArray());

    LOG_TOPIC("f1275", TRACE, arangodb::Logger::ENGINES)
        << "processing collections metadata in database '" << vocbase->name()
        << "': " << slice.toJson();

    for (VPackSlice it : VPackArrayIterator(slice)) {
      // we found a collection that is still active
      LOG_TOPIC("b2ef2", TRACE, arangodb::Logger::ENGINES)
          << "processing collection metadata in database '" << vocbase->name()
          << "': " << it.toJson();

      TRI_ASSERT(!it.get("id").isNone() || !it.get("cid").isNone());
      TRI_ASSERT(!it.get("deleted").isTrue());

      auto collection = vocbase->createCollectionObject(it, /*isAStub*/ false);
      TRI_ASSERT(collection != nullptr);

      auto phy = static_cast<RocksDBCollection*>(collection->getPhysical());
      TRI_ASSERT(phy != nullptr);
      Result r = phy->meta().deserializeMeta(_db, *collection);
      if (r.fail()) {
        LOG_TOPIC("4a404", ERR, arangodb::Logger::ENGINES)
            << "error while "
            << "loading metadata of collection '" << vocbase->name() << "/"
            << collection->name() << "': " << r.errorMessage();
      }

      StorageEngine::registerCollection(*vocbase, collection);
      LOG_TOPIC("39404", DEBUG, arangodb::Logger::ENGINES)
          << "added collection '" << vocbase->name() << "/"
          << collection->name() << "'";
    }
  } catch (std::exception const& ex) {
    LOG_TOPIC("8d427", ERR, arangodb::Logger::ENGINES)
        << "error while opening database '" << vocbase->name()
        << "': " << ex.what();
    throw;
  } catch (...) {
    LOG_TOPIC("0268e", ERR, arangodb::Logger::ENGINES)
        << "error while opening database '" << vocbase->name()
        << "': unknown exception";
    throw;
  }

  // scan the database path for "search-alias" views
  if (ServerState::instance()->isSingleServer()) {
    scanViews(iresearch::StaticStrings::ViewSearchAliasType);
  }

  return vocbase;
}

void RocksDBEngine::loadReplicatedStates(TRI_vocbase_t& vocbase) {
  auto* cfDefs = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Definitions);

  auto bounds = RocksDBKeyBounds::DatabaseStates(vocbase.id());
  auto upper = bounds.end();
  auto readOptions = rocksdb::ReadOptions();
  readOptions.iterate_upper_bound = &upper;

  auto iter =
      std::unique_ptr<rocksdb::Iterator>(_db->NewIterator(readOptions, cfDefs));
  for (iter->Seek(bounds.start()); iter->Valid(); iter->Next()) {
    ADB_PROD_ASSERT(vocbase.id() == RocksDBKey::databaseId(iter->key()));

    auto slice =
        VPackSlice(reinterpret_cast<uint8_t const*>(iter->value().data()));
    // TODO Is this applicable? I think we can safely remove the following
    //      lines.
    if (basics::VelocyPackHelper::getBooleanValue(
            slice, StaticStrings::DataSourceDeleted, false)) {
      TRI_ASSERT(false) << "Please tell Team CINFRA if you see this happening.";
      continue;
    }

    auto info = velocypack::deserialize<
        replication2::storage::rocksdb::ReplicatedStateInfo>(slice);
    auto methods = makeLogStorageMethods(
        info.stateId, info.objectId, vocbase.id(),
        RocksDBColumnFamilyManager::get(
            RocksDBColumnFamilyManager::Family::ReplicatedLogs),
        RocksDBColumnFamilyManager::get(
            RocksDBColumnFamilyManager::Family::Definitions));
    registerReplicatedState(vocbase, info.stateId, std::move(methods));
  }
}

void RocksDBEngine::scheduleFullIndexRefill(std::string const& database,
                                            std::string const& collection,
                                            IndexId iid) {
  // simply forward...
  RocksDBIndexCacheRefillFeature& f =
      server().getFeature<RocksDBIndexCacheRefillFeature>();
  f.scheduleFullIndexRefill(database, collection, iid);
}

bool RocksDBEngine::autoRefillIndexCaches() const {
  RocksDBIndexCacheRefillFeature& f =
      server().getFeature<RocksDBIndexCacheRefillFeature>();
  return f.autoRefill();
}

bool RocksDBEngine::autoRefillIndexCachesOnFollowers() const {
  RocksDBIndexCacheRefillFeature& f =
      server().getFeature<RocksDBIndexCacheRefillFeature>();
  return f.autoRefillOnFollowers();
}

void RocksDBEngine::syncIndexCaches() {
  RocksDBIndexCacheRefillFeature& f =
      server().getFeature<RocksDBIndexCacheRefillFeature>();
  f.waitForCatchup();
}

auto RocksDBEngine::makeLogStorageMethods(
    replication2::LogId logId, uint64_t objectId, std::uint64_t vocbaseId,
    ::rocksdb::ColumnFamilyHandle* const logCf,
    ::rocksdb::ColumnFamilyHandle* const metaCf)
    -> std::unique_ptr<replication2::storage::IStorageEngineMethods> {
#if defined(USE_CUSTOM_WAL)
  auto logPersistor =
      std::make_unique<replication2::storage::wal::LogPersistor>(
          logId, _walManager->createFileManager(logId));
#else
  auto logPersistor =
      std::make_unique<replication2::storage::rocksdb::LogPersistor>(
          logId, objectId, vocbaseId, _db, logCf, _logPersistor, _logMetrics,
          this);
#endif
  auto statePersistor =
      std::make_unique<replication2::storage::rocksdb::StatePersistor>(
          logId, objectId, vocbaseId, _db, metaCf);
  return std::make_unique<replication2::storage::LogStorageMethods>(
      std::move(logPersistor), std::move(statePersistor));
}

DECLARE_GAUGE(rocksdb_cache_active_tables, uint64_t,
              "rocksdb_cache_active_tables");
DECLARE_GAUGE(rocksdb_cache_allocated, uint64_t, "rocksdb_cache_allocated");
DECLARE_GAUGE(rocksdb_cache_peak_allocated, uint64_t,
              "rocksdb_cache_peak_allocated");
DECLARE_GAUGE(rocksdb_cache_hit_rate_lifetime, uint64_t,
              "rocksdb_cache_hit_rate_lifetime");
DECLARE_GAUGE(rocksdb_cache_hit_rate_recent, uint64_t,
              "rocksdb_cache_hit_rate_recent");
DECLARE_GAUGE(rocksdb_cache_limit, uint64_t, "rocksdb_cache_limit");
DECLARE_GAUGE(rocksdb_cache_unused_memory, uint64_t,
              "rocksdb_cache_unused_memory");
DECLARE_GAUGE(rocksdb_cache_unused_tables, uint64_t,
              "rocksdb_cache_unused_tables");
DECLARE_COUNTER(rocksdb_cache_migrate_tasks_total,
                "rocksdb_cache_migrate_tasks_total");
DECLARE_COUNTER(rocksdb_cache_free_memory_tasks_total,
                "rocksdb_cache_free_memory_tasks_total");
DECLARE_COUNTER(rocksdb_cache_migrate_tasks_duration_total,
                "rocksdb_cache_migrate_tasks_duration_total");
DECLARE_COUNTER(rocksdb_cache_free_memory_tasks_duration_total,
                "rocksdb_cache_free_memory_tasks_duration_total");
DECLARE_GAUGE(rocksdb_actual_delayed_write_rate, uint64_t,
              "rocksdb_actual_delayed_write_rate");
DECLARE_GAUGE(rocksdb_background_errors, uint64_t, "rocksdb_background_errors");
DECLARE_GAUGE(rocksdb_base_level, uint64_t, "rocksdb_base_level");
DECLARE_GAUGE(rocksdb_block_cache_capacity, uint64_t,
              "rocksdb_block_cache_capacity");
DECLARE_GAUGE(rocksdb_block_cache_pinned_usage, uint64_t,
              "rocksdb_block_cache_pinned_usage");
DECLARE_GAUGE(rocksdb_block_cache_usage, uint64_t, "rocksdb_block_cache_usage");
#ifdef ARANGODB_ROCKSDB8
// DECLARE_GAUGE(rocksdb_block_cache_entries, uint64_t,
//                    "rocksdb_block_cache_entries");
// DECLARE_GAUGE(rocksdb_block_cache_charge_per_entry, uint64_t,
//                    "rocksdb_block_cache_charge_per_entry");
#endif
DECLARE_GAUGE(rocksdb_compaction_pending, uint64_t,
              "rocksdb_compaction_pending");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level0, uint64_t,
              "rocksdb_compression_ratio_at_level0");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level1, uint64_t,
              "rocksdb_compression_ratio_at_level1");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level2, uint64_t,
              "rocksdb_compression_ratio_at_level2");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level3, uint64_t,
              "rocksdb_compression_ratio_at_level3");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level4, uint64_t,
              "rocksdb_compression_ratio_at_level4");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level5, uint64_t,
              "rocksdb_compression_ratio_at_level5");
DECLARE_GAUGE(rocksdb_compression_ratio_at_level6, uint64_t,
              "rocksdb_compression_ratio_at_level6");
DECLARE_GAUGE(rocksdb_cur_size_active_mem_table, uint64_t,
              "rocksdb_cur_size_active_mem_table");
DECLARE_GAUGE(rocksdb_cur_size_all_mem_tables, uint64_t,
              "rocksdb_cur_size_all_mem_tables");
DECLARE_GAUGE(rocksdb_estimate_live_data_size, uint64_t,
              "rocksdb_estimate_live_data_size");
DECLARE_GAUGE(rocksdb_estimate_num_keys, uint64_t, "rocksdb_estimate_num_keys");
DECLARE_GAUGE(rocksdb_estimate_pending_compaction_bytes, uint64_t,
              "rocksdb_estimate_pending_compaction_bytes");
DECLARE_GAUGE(rocksdb_estimate_table_readers_mem, uint64_t,
              "rocksdb_estimate_table_readers_mem");
DECLARE_GAUGE(rocksdb_free_disk_space, uint64_t, "rocksdb_free_disk_space");
DECLARE_GAUGE(rocksdb_free_inodes, uint64_t, "rocksdb_free_inodes");
DECLARE_GAUGE(rocksdb_is_file_deletions_enabled, uint64_t,
              "rocksdb_is_file_deletions_enabled");
DECLARE_GAUGE(rocksdb_is_write_stopped, uint64_t, "rocksdb_is_write_stopped");
DECLARE_GAUGE(rocksdb_live_sst_files_size, uint64_t,
              "rocksdb_live_sst_files_size");
DECLARE_GAUGE(rocksdb_mem_table_flush_pending, uint64_t,
              "rocksdb_mem_table_flush_pending");
DECLARE_GAUGE(rocksdb_min_log_number_to_keep, uint64_t,
              "rocksdb_min_log_number_to_keep");
DECLARE_GAUGE(rocksdb_num_deletes_active_mem_table, uint64_t,
              "rocksdb_num_deletes_active_mem_table");
DECLARE_GAUGE(rocksdb_num_deletes_imm_mem_tables, uint64_t,
              "rocksdb_num_deletes_imm_mem_tables");
DECLARE_GAUGE(rocksdb_num_entries_active_mem_table, uint64_t,
              "rocksdb_num_entries_active_mem_table");
DECLARE_GAUGE(rocksdb_num_entries_imm_mem_tables, uint64_t,
              "rocksdb_num_entries_imm_mem_tables");
DECLARE_GAUGE(rocksdb_num_files_at_level0, uint64_t,
              "rocksdb_num_files_at_level0");
DECLARE_GAUGE(rocksdb_num_files_at_level1, uint64_t,
              "rocksdb_num_files_at_level1");
DECLARE_GAUGE(rocksdb_num_files_at_level2, uint64_t,
              "rocksdb_num_files_at_level2");
DECLARE_GAUGE(rocksdb_num_files_at_level3, uint64_t,
              "rocksdb_num_files_at_level3");
DECLARE_GAUGE(rocksdb_num_files_at_level4, uint64_t,
              "rocksdb_num_files_at_level4");
DECLARE_GAUGE(rocksdb_num_files_at_level5, uint64_t,
              "rocksdb_num_files_at_level5");
DECLARE_GAUGE(rocksdb_num_files_at_level6, uint64_t,
              "rocksdb_num_files_at_level6");
DECLARE_GAUGE(rocksdb_num_immutable_mem_table, uint64_t,
              "rocksdb_num_immutable_mem_table");
DECLARE_GAUGE(rocksdb_num_immutable_mem_table_flushed, uint64_t,
              "rocksdb_num_immutable_mem_table_flushed");
DECLARE_GAUGE(rocksdb_num_live_versions, uint64_t, "rocksdb_num_live_versions");
DECLARE_GAUGE(rocksdb_num_running_compactions, uint64_t,
              "rocksdb_num_running_compactions");
DECLARE_GAUGE(rocksdb_num_running_flushes, uint64_t,
              "rocksdb_num_running_flushes");
DECLARE_GAUGE(rocksdb_num_snapshots, uint64_t, "rocksdb_num_snapshots");
DECLARE_GAUGE(rocksdb_oldest_snapshot_time, uint64_t,
              "rocksdb_oldest_snapshot_time");
DECLARE_GAUGE(rocksdb_size_all_mem_tables, uint64_t,
              "rocksdb_size_all_mem_tables");
DECLARE_GAUGE(rocksdb_total_disk_space, uint64_t, "rocksdb_total_disk_space");
DECLARE_GAUGE(rocksdb_total_inodes, uint64_t, "rocksdb_total_inodes");
DECLARE_GAUGE(rocksdb_total_sst_files_size, uint64_t,
              "rocksdb_total_sst_files_size");
DECLARE_GAUGE(rocksdb_engine_throttle_bps, uint64_t,
              "rocksdb_engine_throttle_bps");
DECLARE_GAUGE(rocksdb_read_only, uint64_t, "rocksdb_read_only");
DECLARE_GAUGE(rocksdb_total_sst_files, uint64_t, "rocksdb_total_sst_files");

void RocksDBEngine::getCapabilities(velocypack::Builder& builder) const {
  // get generic capabilities
  VPackBuilder main;
  StorageEngine::getCapabilities(main);

  VPackBuilder own;
  own.openObject();
  own.add("endianness", VPackValue(rocksDBEndiannessString(
                            rocksutils::getRocksDBKeyFormatEndianness())));
  own.close();

  VPackCollection::merge(builder, main.slice(), own.slice(), false);
}

void RocksDBEngine::toPrometheus(std::string& result, std::string_view globals,
                                 bool ensureWhitespace) const {
  VPackBuffer<uint8_t> buffer;
  VPackBuilder stats(buffer);
  getStatistics(stats);
  VPackSlice sslice = stats.slice();

  TRI_ASSERT(sslice.isObject());
  for (auto a : VPackObjectIterator(sslice)) {
    if (a.value.isNumber()) {
      std::string name = a.key.copyString();
      std::replace(name.begin(), name.end(), '.', '_');
      std::replace(name.begin(), name.end(), '-', '_');
      if (!name.empty() && name.front() != 'r') {
        // prepend name with "rocksdb_"
        name = absl::StrCat(kEngineName, "_", name);
      }

      metrics::Metric::addInfo(result, name, /*help*/ name,
                               name.ends_with("_total") ? "counter" : "gauge");
      metrics::Metric::addMark(result, name, globals, "");
      absl::StrAppend(&result, ensureWhitespace ? " " : "",
                      a.value.getNumber<uint64_t>(), "\n");
    }
  }
}

void RocksDBEngine::getStatistics(VPackBuilder& builder) const {
  // add int properties
  auto addInt = [&](std::string const& s) {
    std::string v;
    if (_db->GetProperty(s, &v)) {
      int64_t i = basics::StringUtils::int64(v);
      builder.add(s, VPackValue(i));
    }
  };

  // add string properties
  auto addStr = [&](std::string const& s) {
    std::string v;
    if (_db->GetProperty(s, &v)) {
      builder.add(s, VPackValue(v));
    }
  };

  // get string property from each column family and return sum;
  auto addIntAllCf = [&](std::string const& s) {
    int64_t sum = 0;
    std::string v;
    for (auto const cfh : RocksDBColumnFamilyManager::allHandles()) {
      v.clear();
      if (_db->GetProperty(cfh, s, &v)) {
        int64_t temp = basics::StringUtils::int64(v);

        // -1 returned for some things that are valid property but no value
        if (0 < temp) {
          sum += temp;
        }
      }
    }
    builder.add(s, VPackValue(sum));
    return sum;
  };

  // add column family properties
  auto addCf = [&](RocksDBColumnFamilyManager::Family family) {
    std::string name = RocksDBColumnFamilyManager::name(
        family, RocksDBColumnFamilyManager::NameMode::External);
    rocksdb::ColumnFamilyHandle* c = RocksDBColumnFamilyManager::get(family);
    std::string v;
    builder.add(name, VPackValue(VPackValueType::Object));
    if (_db->GetProperty(c, rocksdb::DB::Properties::kCFStats, &v)) {
      builder.add("dbstats", VPackValue(v));
    }

    // re-add this line to count all keys in the column family (slow!!!)
    // builder.add("keys", VPackValue(rocksutils::countKeys(_db, c)));

    // estimate size on disk and in memtables
    uint64_t out = 0;
    rocksdb::Range r(rocksdb::Slice("\x00\x00\x00\x00\x00\x00\x00\x00", 8),
                     rocksdb::Slice("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                                    "\xff\xff\xff\xff\xff\xff",
                                    16));

    rocksdb::SizeApproximationOptions options{.include_memtables = true,
                                              .include_files = true};
    _db->GetApproximateSizes(options, c, &r, 1, &out);

    builder.add("memory", VPackValue(out));
    builder.close();
  };

  builder.openObject(/*unindexed*/ true);
  int64_t numSstFilesOnAllLevels = 0;
  for (int i = 0; i < _optionsProvider.getOptions().num_levels; ++i) {
    numSstFilesOnAllLevels += addIntAllCf(
        absl::StrCat(rocksdb::DB::Properties::kNumFilesAtLevelPrefix, i));
    // ratio needs new calculation with all cf, not a simple add operation
    addIntAllCf(absl::StrCat(
        rocksdb::DB::Properties::kCompressionRatioAtLevelPrefix, i));
  }
  builder.add("rocksdb.total-sst-files", VPackValue(numSstFilesOnAllLevels));
  // caution:  you must read rocksdb/db/internal_stats.cc carefully to
  //           determine if a property is for whole database or one column
  //           family
  addIntAllCf(rocksdb::DB::Properties::kNumImmutableMemTable);
  addIntAllCf(rocksdb::DB::Properties::kNumImmutableMemTableFlushed);
  addIntAllCf(rocksdb::DB::Properties::kMemTableFlushPending);
  addIntAllCf(rocksdb::DB::Properties::kCompactionPending);
  addInt(rocksdb::DB::Properties::kBackgroundErrors);
  addIntAllCf(rocksdb::DB::Properties::kCurSizeActiveMemTable);
  addIntAllCf(rocksdb::DB::Properties::kCurSizeAllMemTables);
  addIntAllCf(rocksdb::DB::Properties::kSizeAllMemTables);
  addIntAllCf(rocksdb::DB::Properties::kNumEntriesActiveMemTable);
  addIntAllCf(rocksdb::DB::Properties::kNumEntriesImmMemTables);
  addIntAllCf(rocksdb::DB::Properties::kNumDeletesActiveMemTable);
  addIntAllCf(rocksdb::DB::Properties::kNumDeletesImmMemTables);
  addIntAllCf(rocksdb::DB::Properties::kEstimateNumKeys);
  addIntAllCf(rocksdb::DB::Properties::kEstimateTableReadersMem);
  addInt(rocksdb::DB::Properties::kNumSnapshots);
  addInt(rocksdb::DB::Properties::kOldestSnapshotTime);
  addIntAllCf(rocksdb::DB::Properties::kNumLiveVersions);
  addInt(rocksdb::DB::Properties::kMinLogNumberToKeep);
  addIntAllCf(rocksdb::DB::Properties::kEstimateLiveDataSize);
  addIntAllCf(rocksdb::DB::Properties::kLiveSstFilesSize);
  addStr(rocksdb::DB::Properties::kDBStats);
  addStr(rocksdb::DB::Properties::kSSTables);
  addInt(rocksdb::DB::Properties::kNumRunningCompactions);
  addInt(rocksdb::DB::Properties::kNumRunningFlushes);
  addInt(rocksdb::DB::Properties::kIsFileDeletionsEnabled);
  addIntAllCf(rocksdb::DB::Properties::kEstimatePendingCompactionBytes);
  addInt(rocksdb::DB::Properties::kBaseLevel);
  addInt(rocksdb::DB::Properties::kBlockCacheCapacity);
  addInt(rocksdb::DB::Properties::kBlockCacheUsage);
  addInt(rocksdb::DB::Properties::kBlockCachePinnedUsage);

#ifdef ARANGODB_ROCKSDB8
  auto const& tableOptions = _optionsProvider.getTableOptions();
  if (tableOptions.block_cache != nullptr) {
    auto const& cache = tableOptions.block_cache;
    auto usage = cache->GetUsage();
    auto entries = cache->GetOccupancyCount();
    if (entries > 0) {
      builder.add("rocksdb.block-cache-charge-per-entry",
                  VPackValue(static_cast<uint64_t>(usage / entries)));
    } else {
      builder.add("rocksdb.block-cache-charge-per-entry", VPackValue(0));
    }
    builder.add("rocksdb.block-cache-entries", VPackValue(entries));
  } else {
    builder.add("rocksdb.block-cache-entries", VPackValue(0));
    builder.add("rocksdb.block-cache-charge-per-entry", VPackValue(0));
  }
#endif

  addIntAllCf(rocksdb::DB::Properties::kTotalSstFilesSize);
  addInt(rocksdb::DB::Properties::kActualDelayedWriteRate);
  addInt(rocksdb::DB::Properties::kIsWriteStopped);

  if (_dbOptions.statistics) {
    for (auto const& stat : rocksdb::TickersNameMap) {
      builder.add(
          stat.second,
          VPackValue(_dbOptions.statistics->getTickerCount(stat.first)));
    }

    uint64_t walWrite, flushWrite, compactionWrite, userWrite;
    walWrite = _dbOptions.statistics->getTickerCount(rocksdb::WAL_FILE_BYTES);
    flushWrite =
        _dbOptions.statistics->getTickerCount(rocksdb::FLUSH_WRITE_BYTES);
    compactionWrite =
        _dbOptions.statistics->getTickerCount(rocksdb::COMPACT_WRITE_BYTES);
    userWrite = _dbOptions.statistics->getTickerCount(rocksdb::BYTES_WRITTEN);
    builder.add(
        "rocksdbengine.write.amplification.x100",
        VPackValue((0 != userWrite)
                       ? ((walWrite + flushWrite + compactionWrite) * 100) /
                             userWrite
                       : 100));
  }

  {
    // in-memory cache statistics
    cache::Manager* manager =
        server().getFeature<CacheManagerFeature>().manager();

    std::pair<double, double> rates;
    cache::Manager::MemoryStats stats;
    if (manager != nullptr) {
      // cache turned on
      stats = manager->memoryStats(cache::Cache::triesFast);
      rates = manager->globalHitRates();
    }

    builder.add("cache.limit", VPackValue(stats.globalLimit));
    builder.add("cache.allocated", VPackValue(stats.globalAllocation));
    builder.add("cache.peak-allocated", VPackValue(stats.peakGlobalAllocation));
    builder.add("cache.active-tables", VPackValue(stats.activeTables));
    builder.add("cache.unused-memory", VPackValue(stats.spareAllocation));
    builder.add("cache.unused-tables", VPackValue(stats.spareTables));
    builder.add("cache.migrate-tasks-total", VPackValue(stats.migrateTasks));
    builder.add("cache.free-memory-tasks-total",
                VPackValue(stats.freeMemoryTasks));
    builder.add("cache.migrate-tasks-duration-total",
                VPackValue(stats.migrateTasksDuration));
    builder.add("cache.free-memory-tasks-duration-total",
                VPackValue(stats.freeMemoryTasksDuration));
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // only here for debugging. the value is not exposed in non-maintainer
    // mode builds. the reason for this is to make calls to the `table` function
    // more lightweight, and because we would need to put a metrics
    // description into Documentation/Metrics for an optional metric.

    // builder.add("cache.table-calls", VPackValue(stats.tableCalls));
    // builder.add("cache.term-calls", VPackValue(stats.termCalls));
#endif

    // edge cache compression ratio
    double compressionRatio = 0.0;
    auto initial = _metricsEdgeCacheEntriesSizeInitial.load();
    auto effective = _metricsEdgeCacheEntriesSizeEffective.load();
    if (initial != 0) {
      compressionRatio = 100.0 * (1.0 - (static_cast<double>(effective) /
                                         static_cast<double>(initial)));
    }
    builder.add("cache.edge-compression-ratio", VPackValue(compressionRatio));

    // handle NaN
    builder.add("cache.hit-rate-lifetime",
                VPackValue(rates.first >= 0.0 ? rates.first : 0.0));
    builder.add("cache.hit-rate-recent",
                VPackValue(rates.second >= 0.0 ? rates.second : 0.0));
  }

  // print column family statistics
  //  warning: output format limits numbers to 3 digits of precision or less.
  builder.add("columnFamilies", VPackValue(VPackValueType::Object));
  addCf(RocksDBColumnFamilyManager::Family::Definitions);
  addCf(RocksDBColumnFamilyManager::Family::Documents);
  addCf(RocksDBColumnFamilyManager::Family::PrimaryIndex);
  addCf(RocksDBColumnFamilyManager::Family::EdgeIndex);
  addCf(RocksDBColumnFamilyManager::Family::VPackIndex);
  addCf(RocksDBColumnFamilyManager::Family::GeoIndex);
  addCf(RocksDBColumnFamilyManager::Family::FulltextIndex);
  addCf(RocksDBColumnFamilyManager::Family::MdiIndex);
  addCf(RocksDBColumnFamilyManager::Family::ReplicatedLogs);
  addCf(RocksDBColumnFamilyManager::Family::MdiVPackIndex);
  if (isVectorIndexEnabled()) {
    addCf(RocksDBColumnFamilyManager::Family::VectorIndex);
  }
  builder.close();

  if (_throttleListener) {
    builder.add("rocksdb_engine.throttle.bps",
                VPackValue(_throttleListener->getThrottle()));
  }

  {
    // total disk space in database directory
    uint64_t totalSpace = 0;
    // free disk space in database directory
    uint64_t freeSpace = 0;
    Result res = TRI_GetDiskSpaceInfo(_basePath, totalSpace, freeSpace);
    if (res.ok()) {
      builder.add("rocksdb.free-disk-space", VPackValue(freeSpace));
      builder.add("rocksdb.total-disk-space", VPackValue(totalSpace));
    } else {
      builder.add("rocksdb.free-disk-space", VPackValue(VPackValueType::Null));
      builder.add("rocksdb.total-disk-space", VPackValue(VPackValueType::Null));
    }
  }

  {
    // total inodes for database directory
    uint64_t totalINodes = 0;
    // free inodes for database directory
    uint64_t freeINodes = 0;
    Result res = TRI_GetINodesInfo(_basePath, totalINodes, freeINodes);
    if (res.ok()) {
      builder.add("rocksdb.free-inodes", VPackValue(freeINodes));
      builder.add("rocksdb.total-inodes", VPackValue(totalINodes));
    } else {
      builder.add("rocksdb.free-inodes", VPackValue(VPackValueType::Null));
      builder.add("rocksdb.total-inodes", VPackValue(VPackValueType::Null));
    }
  }

  if (_errorListener) {
    builder.add("rocksdb.read-only",
                VPackValue(_errorListener->called() ? 1 : 0));
  }

  auto sequenceNumber = _db->GetLatestSequenceNumber();
  builder.add("rocksdb.wal-sequence", VPackValue(sequenceNumber));

  builder.close();
}

Result RocksDBEngine::handleSyncKeys(DatabaseInitialSyncer& syncer,
                                     LogicalCollection& col,
                                     std::string const& keysId) {
  return handleSyncKeysRocksDB(syncer, &col, keysId);
}

Result RocksDBEngine::createLoggerState(TRI_vocbase_t* vocbase,
                                        VPackBuilder& builder) {
  builder.openObject();  // Base
  rocksdb::SequenceNumber lastTick = _db->GetLatestSequenceNumber();

  // "state" part
  builder.add("state", VPackValue(VPackValueType::Object));  // open

  // always hard-coded to true
  builder.add("running", VPackValue(true));

  builder.add("lastLogTick", VPackValue(std::to_string(lastTick)));

  // not used anymore in 3.8:
  builder.add("lastUncommittedLogTick", VPackValue(std::to_string(lastTick)));

  // not used anymore in 3.8:
  builder.add("totalEvents", VPackValue(lastTick));

  builder.add("time", VPackValue(utilities::timeString()));
  builder.close();

  // "server" part
  builder.add("server", VPackValue(VPackValueType::Object));  // open
  builder.add("version", VPackValue(ARANGODB_VERSION));
  builder.add("serverId",
              VPackValue(std::to_string(ServerIdFeature::getId().id())));
  builder.add("engine", VPackValue(kEngineName));  // "rocksdb"
  builder.close();

  // "clients" part
  builder.add("clients", VPackValue(VPackValueType::Array));  // open
  if (vocbase != nullptr) {
    vocbase->replicationClients().toVelocyPack(builder);
  }
  builder.close();  // clients
  builder.close();  // base

  return {};
}

Result RocksDBEngine::createTickRanges(VPackBuilder& builder) {
  rocksdb::VectorLogPtr walFiles;
  rocksdb::Status s = _db->GetSortedWalFiles(walFiles);

  Result res = rocksutils::convertStatus(s);
  if (res.fail()) {
    return res;
  }

  builder.openArray();
  for (auto lfile = walFiles.begin(); lfile != walFiles.end(); ++lfile) {
    auto& logfile = *lfile;
    builder.openObject();
    // filename and state are already of type string
    builder.add("datafile", VPackValue(logfile->PathName()));
    if (logfile->Type() == rocksdb::WalFileType::kAliveLogFile) {
      builder.add("status", VPackValue("open"));
    } else if (logfile->Type() == rocksdb::WalFileType::kArchivedLogFile) {
      builder.add("status", VPackValue("collected"));
    }
    rocksdb::SequenceNumber min = logfile->StartSequence();
    builder.add("tickMin", VPackValue(std::to_string(min)));
    rocksdb::SequenceNumber max;
    if (std::next(lfile) != walFiles.end()) {
      max = (*std::next(lfile))->StartSequence();
    } else {
      max = _db->GetLatestSequenceNumber();
    }
    builder.add("tickMax", VPackValue(std::to_string(max)));
    builder.close();
  }
  builder.close();

  return {};
}

Result RocksDBEngine::firstTick(uint64_t& tick) {
  rocksdb::VectorLogPtr walFiles;
  rocksdb::Status s = _db->GetSortedWalFiles(walFiles);

  Result res;
  if (!s.ok()) {
    res = rocksutils::convertStatus(s);
  } else {
    // read minium possible tick
    if (!walFiles.empty()) {
      tick = walFiles[0]->StartSequence();
    }
  }
  return res;
}

Result RocksDBEngine::lastLogger(TRI_vocbase_t& vocbase, uint64_t tickStart,
                                 uint64_t tickEnd, VPackBuilder& builder) {
  bool includeSystem = true;
  size_t chunkSize = 32 * 1024 * 1024;  // TODO: determine good default value?

  builder.openArray();
  RocksDBReplicationResult rep =
      rocksutils::tailWal(&vocbase, tickStart, tickEnd, chunkSize,
                          includeSystem, DataSourceId::none(), builder);
  builder.close();

  return std::move(rep).result();
}

WalAccess const* RocksDBEngine::walAccess() const {
  TRI_ASSERT(_walAccess);
  return _walAccess.get();
}

/// @brief get compression supported by RocksDB
std::string RocksDBEngine::getCompressionSupport() const {
  std::string result;

  for (auto const& type : rocksdb::GetSupportedCompressions()) {
    std::string out;
    rocksdb::GetStringFromCompressionType(&out, type);

    if (out.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.append(", ");
    }
    result.append(out);
  }
  return result;
}

// management methods for synchronizing with external persistent stores
TRI_voc_tick_t RocksDBEngine::currentTick() const {
  return _db->GetLatestSequenceNumber();
}

TRI_voc_tick_t RocksDBEngine::releasedTick() const {
  READ_LOCKER(lock, _walFileLock);
  return _releasedTick;
}

void RocksDBEngine::releaseTick(TRI_voc_tick_t tick) {
  WRITE_LOCKER(lock, _walFileLock);

  if (tick > _releasedTick) {
    _releasedTick = tick;
    lock.unlock();

    // update metric for released tick
    _metricsWalReleasedTickFlush.store(tick, std::memory_order_relaxed);
  }
}

HealthData RocksDBEngine::healthCheck() {
  auto now = std::chrono::steady_clock::now();

  // the following checks are executed under a mutex so that different
  // threads can potentially call in here without messing up any data.
  // in addition, serializing access to this function avoids stampedes
  // with multiple threads trying to calculate the free disk space
  // capacity at the same time, which could be expensive.
  std::lock_guard guard{_healthMutex};

  TRI_IF_FAILURE("RocksDBEngine::healthCheck") {
    _healthData.res.reset(TRI_ERROR_DEBUG, "peng! 💥");
    return _healthData;
  }

  bool lastCheckLongAgo =
      (_healthData.lastCheckTimestamp.time_since_epoch().count() == 0) ||
      ((now - _healthData.lastCheckTimestamp) >= std::chrono::seconds(30));
  if (lastCheckLongAgo) {
    _healthData.lastCheckTimestamp = now;
  }

  // only log about once every 24 hours, to reduce log spam
  bool lastLogMessageLongAgo =
      (_lastHealthLogMessageTimestamp.time_since_epoch().count() == 0) ||
      ((now - _lastHealthLogMessageTimestamp) >= std::chrono::hours(24));

  _healthData.backgroundError = hasBackgroundError();

  if (_healthData.backgroundError) {
    // go into failed state
    _healthData.res.reset(
        TRI_ERROR_FAILED,
        "storage engine reports background error. please check the logs "
        "for the error reason and take action");
  } else if (_lastHealthCheckSuccessful) {
    _healthData.res.reset();
  }

  if (lastCheckLongAgo || !_lastHealthCheckSuccessful) {
    // check the amount of free disk space. this may be expensive to do, so
    // we only execute the check every once in a while, or when the last check
    // failed too (so that we don't report success only because we skipped the
    // checks)
    //
    // total disk space in database directory
    uint64_t totalSpace = 0;
    // free disk space in database directory
    uint64_t freeSpace = 0;

    if (TRI_GetDiskSpaceInfo(_basePath, totalSpace, freeSpace).ok() &&
        totalSpace >= 1024 * 1024) {
      // only carry out the following if we get a disk size of at least 1MB
      // back. everything else seems to be very unreasonable and not
      // trustworthy.
      double diskFreePercentage = double(freeSpace) / double(totalSpace);
      _healthData.freeDiskSpaceBytes = freeSpace;
      _healthData.freeDiskSpacePercent = diskFreePercentage;

      if (_healthData.res.ok() &&
          ((_requiredDiskFreePercentage > 0.0 &&
            diskFreePercentage < _requiredDiskFreePercentage) ||
           (_requiredDiskFreeBytes > 0 &&
            freeSpace < _requiredDiskFreeBytes))) {
        std::stringstream ss;
        ss << "free disk space capacity has reached critical level, "
           << "bytes free: " << freeSpace
           << ", % free: " << std::setprecision(1) << std::fixed
           << (diskFreePercentage * 100.0);
        // go into failed state
        _healthData.res.reset(TRI_ERROR_FAILED, ss.str());
      } else if (diskFreePercentage < 0.05 || freeSpace < 256 * 1024 * 1024) {
        // warnings about disk space only every 15 minutes
        bool lastLogWarningLongAgo =
            (now - _lastHealthLogWarningTimestamp >= std::chrono::minutes(15));
        if (lastLogWarningLongAgo) {
          LOG_TOPIC("54e7f", WARN, Logger::ENGINES)
              << "free disk space capacity is low, "
              << "bytes free: " << freeSpace
              << ", % free: " << std::setprecision(1) << std::fixed
              << (diskFreePercentage * 100.0);
          _lastHealthLogWarningTimestamp = now;
        }
        // don't go into failed state (yet)
      }
    }
  }

  _lastHealthCheckSuccessful = _healthData.res.ok();

  if (_healthData.res.fail() && lastLogMessageLongAgo) {
    LOG_TOPIC("ead1f", ERR, Logger::ENGINES) << _healthData.res.errorMessage();

    // update timestamp of last log message
    _lastHealthLogMessageTimestamp = now;
  }

  return _healthData;
}

void RocksDBEngine::waitForCompactionJobsToFinish() {
  // wait for started compaction jobs to finish
  int iterations = 0;

  do {
    size_t numRunning;
    {
      READ_LOCKER(locker, _pendingCompactionsLock);
      numRunning = _runningCompactions;
    }
    if (numRunning == 0) {
      return;
    }

    // print this only every few seconds
    if (iterations++ % 200 == 0) {
      LOG_TOPIC("9cbfd", INFO, Logger::ENGINES)
          << "waiting for " << numRunning << " compaction job(s) to finish...";
    }
    // Maybe a flush can help?
    if (iterations == 100) {
      Result res =
          flushWal(false /* waitForSync */, true /* flushColumnFamilies */);
      if (res.fail()) {
        LOG_TOPIC("25161", WARN, Logger::ENGINES)
            << "Error on flushWal during waitForCompactionJobsToFinish: "
            << res.errorMessage();
      }
    }
    // unfortunately there is not much we can do except waiting for
    // RocksDB's compaction job(s) to finish.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // wait up to 2 minutes, and then give up
  } while (iterations <= 2400);

  LOG_TOPIC("c537b", WARN, Logger::ENGINES)
      << "giving up waiting for pending compaction job(s)";
}

bool RocksDBEngine::checkExistingDB(
    std::vector<rocksdb::ColumnFamilyDescriptor> const& cfFamilies) {
  bool dbExisted = false;

  rocksdb::Options testOptions;
  testOptions.create_if_missing = false;
  testOptions.create_missing_column_families = false;
  testOptions.avoid_flush_during_recovery = true;
  testOptions.avoid_flush_during_shutdown = true;
  testOptions.env = _dbOptions.env;

  std::vector<std::string> existingColumnFamilies;
  rocksdb::Status status = rocksdb::DB::ListColumnFamilies(
      testOptions, _path, &existingColumnFamilies);
  if (!status.ok()) {
    // check if we have found the database directory or not
    Result res = rocksutils::convertStatus(status);
    if (res.isNot(TRI_ERROR_ARANGO_IO_ERROR)) {
      // not an I/O error. so we better report the error and abort here
      LOG_TOPIC("74b7f", FATAL, arangodb::Logger::STARTUP)
          << "unable to initialize RocksDB engine: " << res.errorMessage();
      FATAL_ERROR_EXIT();
    }
  }

  if (status.ok()) {
    dbExisted = true;
    // we were able to open the database.
    // now check which column families are present in the db
    std::string names;
    for (auto const& it : existingColumnFamilies) {
      if (!names.empty()) {
        names.append(", ");
      }
      names.append(it);
    }

    LOG_TOPIC("528b8", DEBUG, arangodb::Logger::STARTUP)
        << "found existing column families: " << names;
    auto const replicatedLogsName = RocksDBColumnFamilyManager::name(
        RocksDBColumnFamilyManager::Family::ReplicatedLogs);
    auto const mdiIndexName = RocksDBColumnFamilyManager::name(
        RocksDBColumnFamilyManager::Family::MdiIndex);
    auto const mdiVPackIndexName = RocksDBColumnFamilyManager::name(
        RocksDBColumnFamilyManager::Family::MdiVPackIndex);
    auto const vectorVPackIndexName = RocksDBColumnFamilyManager::name(
        RocksDBColumnFamilyManager::Family::VectorIndex);

    for (auto const& it : cfFamilies) {
      auto it2 = std::find(existingColumnFamilies.begin(),
                           existingColumnFamilies.end(), it.name);
      if (it2 == existingColumnFamilies.end()) {
        if (it.name == replicatedLogsName || it.name == mdiIndexName ||
            it.name == mdiVPackIndexName ||
            (isVectorIndexEnabled() && it.name == vectorVPackIndexName)) {
          LOG_TOPIC("293c3", INFO, Logger::STARTUP)
              << "column family " << it.name
              << " is missing and will be created.";
          continue;
        }

        LOG_TOPIC("d9df8", FATAL, arangodb::Logger::STARTUP)
            << "column family '" << it.name << "' is missing in database"
            << ". if you are upgrading from an earlier alpha or beta version "
               "of ArangoDB, it is required to restart with a new database "
               "directory and "
               "re-import data";
        FATAL_ERROR_EXIT();
      }
    }

    if (existingColumnFamilies.size() <
        RocksDBColumnFamilyManager::minNumberOfColumnFamilies) {
      LOG_TOPIC("e99ec", FATAL, arangodb::Logger::STARTUP)
          << "unexpected number of column families found in database ("
          << existingColumnFamilies.size() << "). "
          << "expecting at least "
          << RocksDBColumnFamilyManager::minNumberOfColumnFamilies
          << ". if you are upgrading from an earlier alpha or beta version "
             "of ArangoDB, it is required to restart with a new database "
             "directory and "
             "re-import data";
      FATAL_ERROR_EXIT();
    }
  }

  return dbExisted;
}

Result RocksDBEngine::dropReplicatedState(
    TRI_vocbase_t& vocbase,
    std::unique_ptr<replication2::storage::IStorageEngineMethods>& ptr) {
  // make sure that all pending async operations have completed
  ptr->waitForCompletion();

  // drop everything
  if (auto res = ptr->drop(); res.fail()) {
    return res;
  }

  // do a compaction for the log range
  std::ignore = ptr->compact();
  ptr.reset();
  return {};
}

auto RocksDBEngine::createReplicatedState(
    TRI_vocbase_t& vocbase, arangodb::replication2::LogId id,
    replication2::storage::PersistedStateInfo const& info)
    -> ResultT<std::unique_ptr<replication2::storage::IStorageEngineMethods>> {
  auto objectId = TRI_NewTickServer();
  auto methods = makeLogStorageMethods(
      id, objectId, vocbase.id(),
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::ReplicatedLogs),
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Definitions));
  auto res = methods->updateMetadata(info);
  if (res.fail()) {
    return res;
  }
  return {std::move(methods)};
}

std::shared_ptr<StorageSnapshot> RocksDBEngine::currentSnapshot() {
  if (ADB_LIKELY(_db)) {
    return std::make_shared<RocksDBSnapshot>(*_db);
  } else {
    return nullptr;
  }
}

std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>
RocksDBEngine::getCacheMetrics() {
  return {_metricsEdgeCacheEntriesSizeInitial.load(),
          _metricsEdgeCacheEntriesSizeEffective.load(),
          _metricsEdgeCacheInserts.load(),
          _metricsEdgeCacheCompressedInserts.load(),
          _metricsEdgeCacheEmptyInserts.load()};
}

void RocksDBEngine::addCacheMetrics(uint64_t initial, uint64_t effective,
                                    uint64_t totalInserts,
                                    uint64_t totalCompressedInserts,
                                    uint64_t totalEmptyInserts) noexcept {
  if (totalInserts > 0) {
    _metricsEdgeCacheEntriesSizeInitial.count(initial);
    _metricsEdgeCacheEntriesSizeEffective.count(effective);
    _metricsEdgeCacheInserts.count(totalInserts);
    _metricsEdgeCacheCompressedInserts.count(totalCompressedInserts);
    _metricsEdgeCacheEmptyInserts.count(totalEmptyInserts);
  }
}

using SortingMethod = arangodb::basics::VelocyPackHelper::SortingMethod;

Result RocksDBEngine::writeSortingFile(SortingMethod sortingMethod) {
  auto& databasePathFeature = server().getFeature<DatabasePathFeature>();
  std::string path = databasePathFeature.subdirectoryName(kSortingMethodFile);
  std::string value =
      sortingMethod == SortingMethod::Legacy ? "LEGACY" : "CORRECT";
  try {
    basics::FileUtils::spit(path, value, true);
  } catch (std::exception const& ex) {
    LOG_TOPIC("8ff0f", ERR, Logger::STARTUP)
        << "unable to write 'SORTING' file '" << path << "': " << ex.what()
        << ". Please make sure the file/directory is writable for the "
           "arangod process and user!";
    return {TRI_ERROR_CANNOT_WRITE_FILE};
  }
  return {};
}

SortingMethod RocksDBEngine::readSortingFile() {
  SortingMethod sortingMethod = SortingMethod::Legacy;
  auto& databasePathFeature = server().getFeature<DatabasePathFeature>();
  std::string path = databasePathFeature.subdirectoryName(kSortingMethodFile);
  std::string value;
  try {
    basics::FileUtils::slurp(path, value);
    value = arangodb::basics::StringUtils::trim(value);
    sortingMethod =
        (value == "LEGACY") ? SortingMethod::Legacy : SortingMethod::Correct;
  } catch (std::exception const& ex) {
    // To find out if we are an agent, we need to check if the AgencyFeature
    // is activated! Note that we cannot use ServerState::isAgent here for
    // the following reason: When we run an upgraded version for the first
    // time, we almost certainly run with --database.auto-upgrade=true .
    // But in this case, the AgencyFeature and the ClusterFeature are
    // disabled! Therefore, the role of the server is not correctly
    // reported to the ServerState class!
    auto& agencyFeature = server().getFeature<AgencyFeature>();
    // When we see a database directory without SORTING file, we fall back
    // to legacy mode, except for agents. Since agents have never used
    // VPackIndexes before we fixed the sorting order, we might as well
    // directly consider them to be migrated to the CORRECT sorting order:
    sortingMethod = agencyFeature.activated() ? SortingMethod::Correct
                                              : SortingMethod::Legacy;
    LOG_TOPIC("8ff0e", WARN, Logger::STARTUP)
        << "unable to read 'SORTING' file '" << path << "': " << ex.what()
        << ". This is expected directly after an upgrade and will then be "
           "rectified automatically for subsequent restarts.";
    if (writeSortingFile(sortingMethod).fail()) {
      LOG_TOPIC("8ff0d", WARN, Logger::STARTUP)
          << "Unable to write 'SORTING' file '" << _path << "', "
          << "this is OK for now, legacy sorting will be used anyway.";
    }
  }
  return sortingMethod;
}

std::string RocksDBEngine::getSortingMethodFile() const {
  return arangodb::basics::FileUtils::buildFilename(_basePath,
                                                    kSortingMethodFile);
}

std::string RocksDBEngine::getLanguageFile() const {
  return arangodb::basics::FileUtils::buildFilename(_basePath, kLanguageFile);
}

}  // namespace arangodb