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

#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "Basics/ReadWriteLock.h"
#include "Basics/VelocyPackHelper.h"
#include "Containers/FlatHashSet.h"
#include "Metrics/Fwd.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "StorageEngine/StorageEngine.h"
#include "VocBase/AccessMode.h"
#include "VocBase/Identifiers/DataSourceId.h"
#include "VocBase/Identifiers/IndexId.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBEngineEE.h"
#endif

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/snapshot.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

namespace rocksdb {
class EncryptionProvider;
class Env;
class TransactionDB;
}  // namespace rocksdb

namespace arangodb {

namespace replication2::storage {
namespace rocksdb {
struct AsyncLogWriteBatcherMetrics;
struct IAsyncLogWriteBatcher;
}  // namespace rocksdb

namespace wal {
struct WalManager;
}
}  // namespace replication2::storage

class PhysicalCollection;
class RocksDBBackgroundErrorListener;
class RocksDBBackgroundThread;
class RocksDBDumpManager;
class RocksDBKey;
class RocksDBLogValue;
class RocksDBRateLimiterThread;
class RocksDBRecoveryHelper;
class RocksDBReplicationManager;
class RocksDBSettingsManager;
class RocksDBSyncThread;
class RocksDBWalAccess;
class TransactionCollection;
class TransactionState;

namespace rest {
class RestHandlerFactory;
}

namespace transaction {
struct Options;
}  // namespace transaction

class RocksDBEngine;  // forward
struct RocksDBOptionsProvider;

/// @brief helper class to make file-purging thread-safe
/// while there is an object of this type around, it will prevent
/// purging of maybe-needed WAL files via holding a lock in the
/// RocksDB engine. if there is no object of this type around,
/// purging is allowed to happen
class RocksDBFilePurgePreventer {
 public:
  RocksDBFilePurgePreventer(RocksDBFilePurgePreventer const&) = delete;
  RocksDBFilePurgePreventer& operator=(RocksDBFilePurgePreventer const&) =
      delete;
  RocksDBFilePurgePreventer& operator=(RocksDBFilePurgePreventer&&) = delete;

  explicit RocksDBFilePurgePreventer(RocksDBEngine*);
  RocksDBFilePurgePreventer(RocksDBFilePurgePreventer&&);
  ~RocksDBFilePurgePreventer();

 private:
  RocksDBEngine* _engine;
};

/// @brief helper class to make file-purging thread-safe
/// creating an object of this type will try to acquire a lock that rules
/// out all WAL iteration/WAL tailing while the lock is held. While this
/// is the case, we are allowed to purge any WAL file, because no other
/// thread is able to access it. Note that it is still safe to delete
/// unneeded WAL files, as they will not be accessed by any other thread.
/// however, without this object it would be unsafe to delete WAL files
/// that may still be accessed by WAL tailing etc.
class RocksDBFilePurgeEnabler {
 public:
  RocksDBFilePurgeEnabler(RocksDBFilePurgePreventer const&) = delete;
  RocksDBFilePurgeEnabler& operator=(RocksDBFilePurgeEnabler const&) = delete;
  RocksDBFilePurgeEnabler& operator=(RocksDBFilePurgeEnabler&&) = delete;

  explicit RocksDBFilePurgeEnabler(RocksDBEngine*);
  RocksDBFilePurgeEnabler(RocksDBFilePurgeEnabler&&);
  ~RocksDBFilePurgeEnabler();

  /// @brief returns true if purging any type of WAL file is currently allowed
  bool canPurge() const { return _engine != nullptr; }

 private:
  RocksDBEngine* _engine;
};

struct ICompactKeyRange {
  virtual ~ICompactKeyRange() = default;
  virtual void compactRange(RocksDBKeyBounds bounds) = 0;
};

class RocksDBEngine final : public StorageEngine, public ICompactKeyRange {
  friend class RocksDBFilePurgePreventer;
  friend class RocksDBFilePurgeEnabler;

 public:
  static constexpr std::string_view kEngineName = "rocksdb";

  static constexpr std::string_view name() noexcept { return "RocksDBEngine"; }

  // create the storage engine
  RocksDBEngine(Server& server, RocksDBOptionsProvider const& optionsProvider,
                metrics::MetricsFeature& metrics);
  ~RocksDBEngine();

  // inherited from ApplicationFeature
  // ---------------------------------

  // add the storage engine's specific options to the global list of options
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override;
  // validate the storage engine's specific options
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override;

  // preparation phase for storage engine. can be used for internal setup.
  // the storage engine must not start any threads here or write any files
  void prepare() override;
  void start() override;
  void beginShutdown() override;
  void stop() override;
  void unprepare() override;

  void flushOpenFilesIfRequired();
  HealthData healthCheck() override;

  std::unique_ptr<transaction::Manager> createTransactionManager(
      transaction::ManagerFeature&) override;
  std::shared_ptr<TransactionState> createTransactionState(
      TRI_vocbase_t& vocbase, TransactionId,
      transaction::Options const& options,
      transaction::OperationOrigin operationOrigin) override;

  // create storage-engine specific collection
  std::unique_ptr<PhysicalCollection> createPhysicalCollection(
      LogicalCollection& collection, velocypack::Slice info) override;

  void getCapabilities(velocypack::Builder& builder) const override;
  void getStatistics(velocypack::Builder& builder) const override;
  void toPrometheus(std::string& result, std::string_view globals,
                    bool ensureWhitespace) const override;

  // inventory functionality
  // -----------------------

  void getDatabases(velocypack::Builder& result) override;

  void getCollectionInfo(TRI_vocbase_t& vocbase, DataSourceId cid,
                         velocypack::Builder& result, bool includeIndexes,
                         TRI_voc_tick_t maxTick) override;

  ErrorCode getCollectionsAndIndexes(TRI_vocbase_t& vocbase,
                                     velocypack::Builder& result,
                                     bool wasCleanShutdown,
                                     bool isUpgrade) override;

  ErrorCode getViews(TRI_vocbase_t& vocbase,
                     velocypack::Builder& result) override;

  std::string versionFilename(TRI_voc_tick_t id) const override;
  std::string databasePath() const override { return _basePath; }
  std::string path() const { return _path; }
  std::string idxPath() const { return _idxPath; }

  void cleanupReplicationContexts() override;

  velocypack::Builder getReplicationApplierConfiguration(
      TRI_vocbase_t& vocbase, ErrorCode& status) override;
  velocypack::Builder getReplicationApplierConfiguration(
      ErrorCode& status) override;
  ErrorCode removeReplicationApplierConfiguration(
      TRI_vocbase_t& vocbase) override;
  ErrorCode removeReplicationApplierConfiguration() override;
  ErrorCode saveReplicationApplierConfiguration(TRI_vocbase_t& vocbase,
                                                velocypack::Slice slice,
                                                bool doSync) override;
  ErrorCode saveReplicationApplierConfiguration(velocypack::Slice slice,
                                                bool doSync) override;
  // TODO worker-safety
  Result handleSyncKeys(DatabaseInitialSyncer& syncer, LogicalCollection& col,
                        std::string const& keysId) override;
  Result createLoggerState(TRI_vocbase_t* vocbase,
                           velocypack::Builder& builder) override;
  Result createTickRanges(velocypack::Builder& builder) override;
  Result firstTick(uint64_t& tick) override;
  Result lastLogger(TRI_vocbase_t& vocbase, uint64_t tickStart,
                    uint64_t tickEnd, velocypack::Builder& builder) override;
  WalAccess const* walAccess() const override;

  // database, collection and index management
  // -----------------------------------------

  /// @brief return a list of the currently open WAL files
  std::vector<std::string> currentWalFiles() const override;

  /// @brief flushes the RocksDB WAL.
  /// the optional parameter "waitForSync" is currently only used when the
  /// "flushColumnFamilies" parameter is also set to true. If
  /// "flushColumnFamilies" is true, all the RocksDB column family memtables are
  /// flushed, and, if "waitForSync" is set, additionally synced to disk. The
  /// only call site that uses "flushColumnFamilies" currently is hot backup.
  /// The function parameter name are a remainder from MMFiles times, when
  /// they made more sense. This can be refactored at any point, so that
  /// flushing column families becomes a separate API.
  Result flushWal(bool waitForSync = false,
                  bool flushColumnFamilies = false) override;
  void waitForEstimatorSync() override;

  virtual std::unique_ptr<TRI_vocbase_t> openDatabase(CreateDatabaseInfo&& info,
                                                      bool isUpgrade) override;
  Result writeCreateDatabaseMarker(TRI_voc_tick_t id,
                                   velocypack::Slice slice) override;
  Result prepareDropDatabase(TRI_vocbase_t& vocbase) override;
  Result dropDatabase(TRI_vocbase_t& database) override;

  // wal in recovery
  RecoveryState recoveryState() noexcept override;

  /// @brief current recovery tick
  TRI_voc_tick_t recoveryTick() noexcept override;

  /// @brief disallow purging of WAL files even if the archive gets too big
  /// removing WAL files does not seem to be thread-safe, so we have to track
  /// usage of WAL files ourselves
  RocksDBFilePurgePreventer disallowPurging() noexcept;

  /// @brief whether or not purging of WAL files is currently allowed
  RocksDBFilePurgeEnabler startPurging() noexcept;

  void scheduleTreeRebuild(TRI_voc_tick_t database,
                           std::string const& collection);
  void processTreeRebuilds();

  void compactRange(RocksDBKeyBounds bounds) override;
  void processCompactions();

  auto dropReplicatedState(
      TRI_vocbase_t& vocbase,
      std::unique_ptr<replication2::storage::IStorageEngineMethods>& ptr)
      -> Result override;

  auto createReplicatedState(
      TRI_vocbase_t& vocbase, replication2::LogId id,
      replication2::storage::PersistedStateInfo const& info)
      -> ResultT<std::unique_ptr<
          replication2::storage::IStorageEngineMethods>> override;

  void createCollection(TRI_vocbase_t& vocbase,
                        LogicalCollection const& collection) override;

  void prepareDropCollection(TRI_vocbase_t& vocbase,
                             LogicalCollection& collection) override;
  Result dropCollection(TRI_vocbase_t& vocbase,
                        LogicalCollection& collection) override;

  void changeCollection(TRI_vocbase_t& vocbase,
                        LogicalCollection const& collection) override;

  Result renameCollection(TRI_vocbase_t& vocbase,
                          LogicalCollection const& collection,
                          std::string const& oldName) override;

  Result changeView(LogicalView const& view, velocypack::Slice update) final;

  Result createView(TRI_vocbase_t& vocbase, DataSourceId id,
                    LogicalView const& view) override;

  Result dropView(TRI_vocbase_t const& vocbase,
                  LogicalView const& view) override;

  Result compactAll(bool changeLevel, bool compactBottomMostLevel) override;

  /// @brief Add engine-specific optimizer rules
  void addOptimizerRules(aql::OptimizerRulesFeature& feature) override;

#ifdef USE_V8
  /// @brief Add engine-specific V8 functions
  void addV8Functions() override;
#endif

  /// @brief Add engine-specific REST handlers
  void addRestHandlers(rest::RestHandlerFactory& handlerFactory) override;

  void addParametersForNewCollection(velocypack::Builder& builder,
                                     velocypack::Slice info) override;

  rocksdb::TransactionDB* db() const { return _db; }

  Result writeDatabaseMarker(TRI_voc_tick_t id, velocypack::Slice slice,
                             RocksDBLogValue&& logValue);
  Result writeCreateCollectionMarker(TRI_voc_tick_t databaseId, DataSourceId id,
                                     velocypack::Slice slice,
                                     RocksDBLogValue&& logValue);

  void addCollectionMapping(uint64_t objectId, TRI_voc_tick_t dbId,
                            DataSourceId cid);
  std::vector<std::tuple<uint64_t, TRI_voc_tick_t, DataSourceId>>
  collectionMappings() const;
  void addIndexMapping(uint64_t objectId, TRI_voc_tick_t dbId, DataSourceId cid,
                       IndexId iid);
  void removeIndexMapping(uint64_t objectId);

  // Identifies a collection
  using CollectionPair = std::pair<TRI_voc_tick_t, DataSourceId>;
  using IndexTriple = std::tuple<TRI_voc_tick_t, DataSourceId, IndexId>;
  CollectionPair mapObjectToCollection(uint64_t objectId) const;
  IndexTriple mapObjectToIndex(uint64_t objectId) const;
  void removeCollectionMapping(uint64_t objectId);

  /// @brief determine how many archived WAL files are available. this is called
  /// during the first few minutes after the instance start, when we don't want
  /// to prune any WAL files yet.
  /// this also updates the metrics for the number of available WAL files.
  void determineWalFilesInitial();

  /// @brief determine which archived WAL files are prunable. as a side-effect,
  /// this updates the metrics for the number of available and prunable WAL
  /// files.
  void determinePrunableWalFiles(TRI_voc_tick_t minTickToKeep);
  void pruneWalFiles();

  double pruneWaitTimeInitial() const { return _pruneWaitTimeInitial; }

  // management methods for synchronizing with external persistent stores
  TRI_voc_tick_t currentTick() const override;
  TRI_voc_tick_t releasedTick() const override;
  void releaseTick(TRI_voc_tick_t) override;

  void scheduleFullIndexRefill(std::string const& database,
                               std::string const& collection,
                               IndexId iid) override;

  bool autoRefillIndexCaches() const override;
  bool autoRefillIndexCachesOnFollowers() const override;

  void syncIndexCaches() override;

  /// @brief whether or not the database existed at startup. this function
  /// provides a valid answer only after start() has successfully finished,
  /// so don't call it from other features during their start() if they are
  /// earlier in the startup sequence
  bool dbExisted() const noexcept { return _dbExisted; }

  void trackRevisionTreeHibernation() noexcept;
  void trackRevisionTreeResurrection() noexcept;

  void trackRevisionTreeMemoryIncrease(std::uint64_t value) noexcept;
  void trackRevisionTreeMemoryDecrease(std::uint64_t value) noexcept;

  void trackRevisionTreeBufferedMemoryIncrease(std::uint64_t value) noexcept;
  void trackRevisionTreeBufferedMemoryDecrease(std::uint64_t value) noexcept;

  void trackIndexSelectivityMemoryIncrease(std::uint64_t value) noexcept;
  void trackIndexSelectivityMemoryDecrease(std::uint64_t value) noexcept;

  metrics::Gauge<uint64_t>& indexEstimatorMemoryUsageMetric() const noexcept {
    return _metricsIndexEstimatorMemoryUsage;
  }

  std::string getSortingMethodFile() const;

  std::string getLanguageFile() const;

#ifdef USE_ENTERPRISE
  bool encryptionKeyRotationEnabled() const;

  bool isEncryptionEnabled() const;

  std::string const& getEncryptionKey();

  std::string getEncryptionTypeFile() const;

  std::string getKeyStoreFolder() const;

  std::vector<enterprise::EncryptionSecret> userEncryptionSecrets() const;

  /// rotate user-provided keys, writes out the internal key files
  Result rotateUserEncryptionKeys();

  /// load encryption at rest key from specified keystore
  Result decryptInternalKeystore(
      std::string const& keystorePath,
      std::vector<enterprise::EncryptionSecret>& userKeys,
      std::string& encryptionKey) const;

  void configureEnterpriseRocksDBOptions(rocksdb::DBOptions& options,
                                         bool createdEngineDir);
#endif

  // returns whether sha files are created or not
  bool getCreateShaFiles() const { return _createShaFiles; }

  // enabled or disable sha file creation. Requires feature not be started.
  void setCreateShaFiles(bool create) { _createShaFiles = create; }

  rocksdb::EncryptionProvider* encryptionProvider() const noexcept {
#ifdef USE_ENTERPRISE
    return _eeData._encryptionProvider;
#else
    return nullptr;
#endif
  }

  rocksdb::DBOptions const& rocksDBOptions() const { return _dbOptions; }

  /// @brief recovery manager
  RocksDBSettingsManager* settingsManager() const {
    TRI_ASSERT(_settingsManager);
    return _settingsManager.get();
  }

  /// @brief manages the ongoing dump clients
  RocksDBReplicationManager* replicationManager() const {
    TRI_ASSERT(_replicationManager);
    return _replicationManager.get();
  }

  RocksDBDumpManager* dumpManager() const {
    TRI_ASSERT(_dumpManager);
    return _dumpManager.get();
  }

  /// @brief returns a pointer to the sync thread
  /// note: returns a nullptr if automatic syncing is turned off!
  RocksDBSyncThread* syncThread() const { return _syncThread.get(); }

  bool hasBackgroundError() const;

  static Result registerRecoveryHelper(
      std::shared_ptr<RocksDBRecoveryHelper> helper);
  static std::vector<std::shared_ptr<RocksDBRecoveryHelper>> const&
  recoveryHelpers();

#ifdef ARANGODB_USE_GOOGLE_TESTS
  uint64_t recoveryStartSequence() const noexcept {
    return _recoveryStartSequence;
  }
  void recoveryStartSequence(uint64_t value) noexcept {
    TRI_ASSERT(_recoveryStartSequence == 0);
    _recoveryStartSequence = value;
  }
#endif

  class RocksDBSnapshot final : public StorageSnapshot {
   public:
    explicit RocksDBSnapshot(rocksdb::DB& db) : _snapshot(&db) {}

    TRI_voc_tick_t tick() const noexcept override {
      return _snapshot.snapshot()->GetSequenceNumber();
    }

    decltype(auto) getSnapshot() const { return _snapshot.snapshot(); }

   private:
    mutable rocksdb::ManagedSnapshot _snapshot;
  };

  std::shared_ptr<StorageSnapshot> currentSnapshot() override;

  void addCacheMetrics(uint64_t initial, uint64_t effective,
                       uint64_t totalInserts, uint64_t totalCompressedInserts,
                       uint64_t totalEmptyInserts) noexcept;

  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>
  getCacheMetrics();

 private:
  void loadReplicatedStates(TRI_vocbase_t& vocbase);
  [[nodiscard]] Result dropReplicatedStates(TRI_voc_tick_t databaseId);
  void shutdownRocksDBInstance() noexcept;
  void waitForCompactionJobsToFinish();
  velocypack::Builder getReplicationApplierConfiguration(RocksDBKey const& key,
                                                         ErrorCode& status);
  ErrorCode removeReplicationApplierConfiguration(RocksDBKey const& key);
  ErrorCode saveReplicationApplierConfiguration(RocksDBKey const& key,
                                                velocypack::Slice slice,
                                                bool doSync);
  Result dropDatabase(TRI_voc_tick_t);
  bool systemDatabaseExists();
  void addSystemDatabase();
  /// @brief open an existing database. internal function
  std::unique_ptr<TRI_vocbase_t> openExistingDatabase(CreateDatabaseInfo&& info,
                                                      bool wasCleanShutdown,
                                                      bool isUpgrade);

  std::string getCompressionSupport() const;

  [[noreturn]] void verifySstFiles(rocksdb::Options const& options) const;

  [[nodiscard]] bool isVectorIndexEnabled() const;

#ifdef USE_ENTERPRISE
  void collectEnterpriseOptions(std::shared_ptr<options::ProgramOptions>);
  void validateEnterpriseOptions(std::shared_ptr<options::ProgramOptions>);
  void prepareEnterprise();

  void validateJournalFiles() const;

  Result readUserEncryptionSecrets(
      std::vector<enterprise::EncryptionSecret>& outlist) const;

  enterprise::RocksDBEngineEEData _eeData;

  /// load encryption at rest key from keystore
  Result decryptInternalKeystore();
  /// encrypt the internal keystore with all user keys
  Result encryptInternalKeystore();
#endif

  bool checkExistingDB(
      std::vector<rocksdb::ColumnFamilyDescriptor> const& cfFamilies);

  auto makeLogStorageMethods(replication2::LogId logId, uint64_t objectId,
                             std::uint64_t vocbaseId,
                             ::rocksdb::ColumnFamilyHandle* const logCf,
                             ::rocksdb::ColumnFamilyHandle* const metaCf)
      -> std::unique_ptr<replication2::storage::IStorageEngineMethods>;

 public:
  Result writeSortingFile(
      arangodb::basics::VelocyPackHelper::SortingMethod sortingMethod);

  // The following method returns what is detected for the sorting method.
  // If no SORTING file is detected, a new one with "LEGACY" will be created.
  arangodb::basics::VelocyPackHelper::SortingMethod readSortingFile();
  arangodb::basics::VelocyPackHelper::SortingMethod currentSortingMethod()
      const {
    return _sortingMethod;
  }

 private:
  RocksDBOptionsProvider const& _optionsProvider;

  metrics::MetricsFeature& _metrics;

  /// single rocksdb database used in this storage engine
  rocksdb::TransactionDB* _db;
  /// default read options
  rocksdb::DBOptions _dbOptions;
  /// path used by rocksdb (inside _basePath)
  std::string _path;
  /// path to arangodb data dir
  std::string _basePath;
  /// path used for index creation
  std::string _idxPath;

  /// @brief repository for replication contexts
  std::unique_ptr<RocksDBReplicationManager> _replicationManager;
  /// @brief tracks the count of documents in collections
  std::unique_ptr<RocksDBSettingsManager> _settingsManager;
  /// @brief Local wal access abstraction
  std::unique_ptr<RocksDBWalAccess> _walAccess;

  /// Background thread handling garbage collection etc
  std::unique_ptr<RocksDBBackgroundThread> _backgroundThread;
  uint64_t _maxTransactionSize;       // maximum allowed size for a transaction
  uint64_t _intermediateCommitSize;   // maximum size for a
                                      // transaction before an
                                      // intermediate commit is performed
  uint64_t _intermediateCommitCount;  // limit of transaction count
                                      // for intermediate commit

  uint64_t _maxParallelCompactions;

  // hook-ins for recovery process
  static std::vector<std::shared_ptr<RocksDBRecoveryHelper>> _recoveryHelpers;

  mutable basics::ReadWriteLock _mapLock;
  // object id (of collection) => { database id, data source id }
  std::unordered_map<uint64_t, CollectionPair> _collectionMap;
  std::unordered_map<uint64_t, IndexTriple> _indexMap;

  /// @brief protects _prunableWalFiles
  mutable basics::ReadWriteLock _walFileLock;

  /// @brief which WAL files can be pruned when
  /// an expiration time of <= 0.0 means the file does not have expired, but
  /// still should be purged because the WAL files archive outgrew its max
  /// configured size
  std::unordered_map<std::string, double> _prunableWalFiles;

  // number of seconds to wait before an obsolete WAL file is actually pruned
  double _pruneWaitTime;

  // number of seconds to wait initially after server start before WAL file
  // deletion kicks in
  double _pruneWaitTimeInitial;

  /// @brief maximum total size (in bytes) of archived WAL files
  uint64_t _maxWalArchiveSizeLimit;

  // do not release walfiles containing writes later than this
  TRI_voc_tick_t _releasedTick;

  /// Background thread handling WAL syncing
  /// note: this is a nullptr if automatic syncing is turned off!
  std::unique_ptr<RocksDBSyncThread> _syncThread;

  // WAL sync interval, specified in milliseconds by end user, but uses
  // microseconds internally
  uint64_t _syncInterval;

  // WAL sync delay threshold. Any WAL disk sync longer ago than this value
  // will trigger a warning (in milliseconds)
  uint64_t _syncDelayThreshold;

  /// @brief minimum required percentage of free disk space for considering the
  /// server "healthy". this is expressed as a floating point value between 0
  /// and 1! if set to 0.0, the % amount of free disk is ignored in checks.
  double _requiredDiskFreePercentage;

  /// @brief minimum number of free bytes on disk for considering the server
  /// healthy. if set to 0, the number of free bytes on disk is ignored in
  /// checks.
  uint64_t _requiredDiskFreeBytes;

  // use rate limiting for RocksDB write operations
  bool _useRateLimiter;

  /// @brief whether or not to use _releasedTick when determining the WAL files
  /// to prune
  bool _useReleasedTick;

  /// @brief activate rocksdb's debug logging
  bool _debugLogging;

  /// @brief whether or not to verify the sst files present in the db path
  bool _verifySst;

  /// @brief activate generation of SHA256 files for .sst and .blob files
  bool _createShaFiles;

  /// @brief whether or not the last health check was successful.
  /// this is used to determine when to execute the potentially expensive
  /// checks for free disk space
  bool _lastHealthCheckSuccessful;

  /// @brief whether or not the DB existed at startup
  bool _dbExisted;

  // code to pace ingest rate of writes to reduce chances of compactions getting
  // too far behind and blocking incoming writes
  // (will only be set if _useRateLimiter is true)
  std::shared_ptr<RocksDBRateLimiterThread> _rateLimiter;

  /// @brief background error listener. will be invoked by rocksdb in case of
  /// a non-recoverable error
  std::shared_ptr<RocksDBBackgroundErrorListener> _errorListener;

  basics::ReadWriteLock _purgeLock;

  /// @brief mutex that protects the storage engine health check
  std::mutex _healthMutex;

  /// @brief timestamp of last health check log message. we only log health
  /// check errors every so often, in order to prevent log spamming
  std::chrono::steady_clock::time_point _lastHealthLogMessageTimestamp;

  /// @brief timestamp of last health check warning message. we only log health
  /// check warnings every so often, in order to prevent log spamming
  std::chrono::steady_clock::time_point _lastHealthLogWarningTimestamp;

  /// @brief global health data, updated periodically
  HealthData _healthData;

  /// @brief lock for _rebuildCollections
  std::mutex _rebuildCollectionsLock;
  /// @brief map of database/collection-guids for which we need to repair trees
  std::map<std::pair<TRI_voc_tick_t, std::string>, bool> _rebuildCollections;
  /// @brief number of currently running tree rebuild jobs jobs
  size_t _runningRebuilds;

  /// @brief lock for _pendingCompactions and _runningCompactions
  basics::ReadWriteLock _pendingCompactionsLock;
  /// @brief bounds for compactions that we have to process
  std::deque<RocksDBKeyBounds> _pendingCompactions;
  /// @brief number of currently running compaction jobs
  size_t _runningCompactions;
  /// @brief column families for which we are currently running a compaction.
  /// we track this because we want to avoid running multiple compactions on
  /// the same column family concurrently. this can help to avoid a shutdown
  /// hanger in rocksdb.
  containers::FlatHashSet<rocksdb::ColumnFamilyHandle*>
      _runningCompactionsColumnFamilies;

  // frequency for rate limiter in milliseconds between iterations
  uint64_t _rateLimiterFrequency = 1000;

  // number of historic data slots to keep around for rate limiter
  uint64_t _rateLimiterSlots = 128;
  // adaptiveness factor for rate limiter:
  // following is a heuristic value, determined by trial and error.
  // its job is slow down the rate of change in the current throttle.
  // we do not want sudden changes in one or two intervals to swing
  // the write rate value wildly. the goal is a nice, even value.
  // note that the write rate is reduced by applying the reduction
  // divided by the full scaling factor, and that the write rate is
  // increased by applying the increase divided by half of the
  // scaling factor, i.e.
  // - write_rate = old_write_rate + increase / (scaling_factor / 2)
  // - write_rate = old_write_rate - decrease / scaling_factor
  uint64_t _rateLimiterScalingFactor = 192;
  // min write rate enforced by rate limiter
  uint64_t _rateLimiterMinWriteRate = 10 * 1024 * 1024;
  // max write rate enforced by rate limiter (0 = unlimited)
  uint64_t _rateLimiterMaxWriteRate = 0;

  // sequence number from which WAL recovery was started. used only
  // for testing
#ifdef ARANGODB_USE_GOOGLE_TESTS
  uint64_t _recoveryStartSequence = 0;
#endif

  // last point in time when an auto-flush happened
  std::chrono::steady_clock::time_point _autoFlushLastExecuted;
  // interval (in s) in which auto-flushing is tried
  double _autoFlushCheckInterval;
  // minimum number of live WAL files that need to be present to trigger
  // an auto-flush
  uint64_t _autoFlushMinWalFiles;

  bool _forceLittleEndianKeys;  // force new database to use old format

  metrics::Gauge<uint64_t>& _metricsIndexEstimatorMemoryUsage;
  metrics::Gauge<uint64_t>& _metricsWalReleasedTickFlush;
  metrics::Gauge<uint64_t>& _metricsWalSequenceLowerBound;
  metrics::Gauge<uint64_t>& _metricsLiveWalFiles;
  metrics::Gauge<uint64_t>& _metricsArchivedWalFiles;
  metrics::Gauge<uint64_t>& _metricsLiveWalFilesSize;
  metrics::Gauge<uint64_t>& _metricsArchivedWalFilesSize;
  metrics::Gauge<uint64_t>& _metricsPrunableWalFiles;
  metrics::Gauge<uint64_t>& _metricsWalPruningActive;
  metrics::Gauge<uint64_t>& _metricsTreeMemoryUsage;
  metrics::Gauge<uint64_t>& _metricsTreeBufferedMemoryUsage;
  metrics::Counter& _metricsTreeRebuildsSuccess;
  metrics::Counter& _metricsTreeRebuildsFailure;
  metrics::Counter& _metricsTreeHibernations;
  metrics::Counter& _metricsTreeResurrections;

  // total size of uncompressed values for the edge cache
  metrics::Counter& _metricsEdgeCacheEntriesSizeInitial;
  // total size of values stored in the edge cache (can be smaller than the
  // initial size because of compression)
  metrics::Counter& _metricsEdgeCacheEntriesSizeEffective;

  // total number of inserts into edge cache
  metrics::Counter& _metricsEdgeCacheInserts;
  // total number of inserts into edge cache that were compressed
  metrics::Counter& _metricsEdgeCacheCompressedInserts;
  // total number of inserts into edge cache that stored an empty array
  metrics::Counter& _metricsEdgeCacheEmptyInserts;

  // @brief persistor for replicated logs
  std::shared_ptr<replication2::storage::rocksdb::AsyncLogWriteBatcherMetrics>
      _logMetrics;
  std::shared_ptr<replication2::storage::rocksdb::IAsyncLogWriteBatcher>
      _logPersistor;

  // Checksum env for when creation of sha files is enabled
  // this is for when encryption is enabled, sha files will be created
  // after the encryption of the .sst and .blob files
  std::unique_ptr<rocksdb::Env> _checksumEnv;

  std::unique_ptr<RocksDBDumpManager> _dumpManager;

  std::shared_ptr<replication2::storage::wal::WalManager> _walManager;

  // For command line option to force legacy even for new databases.
  bool _forceLegacySortingMethod;

  arangodb::basics::VelocyPackHelper::SortingMethod
      _sortingMethod;  // Detected at startup in the prepare method
};

static constexpr const char* kEncryptionTypeFile = "ENCRYPTION";
static constexpr const char* kEncryptionKeystoreFolder = "ENCRYPTION-KEYS";
static constexpr const char* kSortingMethodFile = "SORTING";
static constexpr const char* kLanguageFile = "LANGUAGE";

}  // namespace arangodb
