////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ROCKSDB_ENGINE_ROCKSDB_ENGINE_H
#define ARANGOD_ROCKSDB_ENGINE_ROCKSDB_ENGINE_H 1

#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "StorageEngine/StorageEngine.h"
#include "VocBase/AccessMode.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBEngineEE.h"
#endif

#include <rocksdb/options.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

namespace rocksdb {
class TransactionDB;
}

namespace arangodb {
class PhysicalCollection;
class PhysicalView;
class RocksDBBackgroundThread;
class RocksDBCounterManager;
class RocksDBKey;
class RocksDBLogValue;
class RocksDBReplicationManager;
class RocksDBVPackComparator;
class RocksDBWalAccess;
class TransactionCollection;
class TransactionState;

namespace rest {
class RestHandlerFactory;
}

namespace transaction {
class ContextData;
struct Options;
}

class RocksDBEngine final : public StorageEngine {
 public:
  // create the storage engine
  explicit RocksDBEngine(application_features::ApplicationServer*);
  ~RocksDBEngine();

  // inherited from ApplicationFeature
  // ---------------------------------

  // add the storage engine's specifc options to the global list of options
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

  bool supportsDfdb() const override { return false; }
  bool useRawDocumentPointers() override { return false; }

  TransactionManager* createTransactionManager() override;
  transaction::ContextData* createTransactionContextData() override;
  TransactionState* createTransactionState(TRI_vocbase_t*,
                    transaction::Options const&) override;
  TransactionCollection* createTransactionCollection(
      TransactionState* state, TRI_voc_cid_t cid, AccessMode::Type accessType,
      int nestingLevel) override;

  // create storage-engine specific collection
  PhysicalCollection* createPhysicalCollection(LogicalCollection*,
                                               velocypack::Slice const&) override;

  // create storage-engine specific view
  PhysicalView* createPhysicalView(LogicalView*, velocypack::Slice const&) override;

  void getStatistics(velocypack::Builder& builder) const override;

  // inventory functionality
  // -----------------------

  void getDatabases(arangodb::velocypack::Builder& result) override;
  void getCollectionInfo(TRI_vocbase_t* vocbase, TRI_voc_cid_t cid,
                         arangodb::velocypack::Builder& result,
                         bool includeIndexes, TRI_voc_tick_t maxTick) override;
  int getCollectionsAndIndexes(TRI_vocbase_t* vocbase,
                               arangodb::velocypack::Builder& result,
                               bool wasCleanShutdown, bool isUpgrade) override;

  int getViews(TRI_vocbase_t* vocbase,
               arangodb::velocypack::Builder& result) override;

  std::string versionFilename(TRI_voc_tick_t id) const override;
  std::string databasePath(TRI_vocbase_t const* vocbase) const override {
    return _basePath;
  }
  std::string collectionPath(TRI_vocbase_t const* vocbase,
                             TRI_voc_cid_t id) const override {
    return std::string(); // no path to be returned here
  }

  velocypack::Builder getReplicationApplierConfiguration(TRI_vocbase_t* vocbase,
                                                         int& status) override;
  velocypack::Builder getReplicationApplierConfiguration(int& status) override;
  int removeReplicationApplierConfiguration(TRI_vocbase_t* vocbase) override;
  int removeReplicationApplierConfiguration() override;
  int saveReplicationApplierConfiguration(TRI_vocbase_t* vocbase,
                                          arangodb::velocypack::Slice slice,
                                          bool doSync) override;
  int saveReplicationApplierConfiguration(arangodb::velocypack::Slice slice,
                                          bool doSync) override;
  Result handleSyncKeys(arangodb::DatabaseInitialSyncer& syncer,
                        arangodb::LogicalCollection* col,
                        std::string const& keysId, std::string const& cid,
                        std::string const& collectionName, TRI_voc_tick_t maxTick) override;
  Result createLoggerState(TRI_vocbase_t* vocbase,
                           velocypack::Builder& builder) override;
  Result createTickRanges(velocypack::Builder& builder) override;
  Result firstTick(uint64_t& tick) override;
  Result lastLogger(TRI_vocbase_t* vocbase,
                    std::shared_ptr<transaction::Context>, uint64_t tickStart,
                    uint64_t tickEnd,
                    std::shared_ptr<velocypack::Builder>& builderSPtr) override;
  WalAccess const* walAccess() const override;
  
  // database, collection and index management
  // -----------------------------------------
  
  // intentionally empty, not useful for this type of engine
  void waitForSyncTick(TRI_voc_tick_t) override {}
  void waitForSyncTimeout(double) override {}
  Result flushWal(bool waitForSync, bool waitForCollector,
                  bool writeShutdownFile) override;

  virtual TRI_vocbase_t* openDatabase(velocypack::Slice const& parameters,
                                      bool isUpgrade, int&) override;
  TRI_vocbase_t* createDatabase(TRI_voc_tick_t id,
                                arangodb::velocypack::Slice const& args,
                                int& status) override;
  int writeCreateDatabaseMarker(TRI_voc_tick_t id,
                                velocypack::Slice const& slice) override;
  void prepareDropDatabase(TRI_vocbase_t* vocbase, bool useWriteMarker,
                           int& status) override;
  Result dropDatabase(TRI_vocbase_t* database) override;
  void waitUntilDeletion(TRI_voc_tick_t id, bool force, int& status) override;

  // wal in recovery
  bool inRecovery() override;
  // start compactor thread and delete files form collections marked as deleted
  void recoveryDone(TRI_vocbase_t* vocbase) override;

 public:
  std::string createCollection(TRI_vocbase_t* vocbase, TRI_voc_cid_t id,
                               arangodb::LogicalCollection const*) override;

  arangodb::Result persistCollection(
      TRI_vocbase_t* vocbase, arangodb::LogicalCollection const*) override;
  arangodb::Result dropCollection(TRI_vocbase_t* vocbase,
                                  arangodb::LogicalCollection*) override;
  void destroyCollection(TRI_vocbase_t* vocbase,
                         arangodb::LogicalCollection*) override;
  void changeCollection(TRI_vocbase_t* vocbase, TRI_voc_cid_t id,
                        arangodb::LogicalCollection const*,
                        bool doSync) override;
  arangodb::Result renameCollection(TRI_vocbase_t* vocbase,
                                    arangodb::LogicalCollection const*,
                                    std::string const& oldName) override;
  void createIndex(TRI_vocbase_t* vocbase, TRI_voc_cid_t collectionId,
                   TRI_idx_iid_t id,
                   arangodb::velocypack::Slice const& data) override;
  void unloadCollection(TRI_vocbase_t* vocbase,
                        arangodb::LogicalCollection* collection) override;
  void createView(TRI_vocbase_t* vocbase, TRI_voc_cid_t id,
                  arangodb::LogicalView const*) override;

  arangodb::Result persistView(TRI_vocbase_t* vocbase,
                               arangodb::LogicalView const*) override;

  arangodb::Result dropView(TRI_vocbase_t* vocbase,
                            arangodb::LogicalView*) override;
  void destroyView(TRI_vocbase_t* vocbase, arangodb::LogicalView*) override;
  void changeView(TRI_vocbase_t* vocbase, TRI_voc_cid_t id,
                  arangodb::LogicalView const*, bool doSync) override;
  void signalCleanup(TRI_vocbase_t* vocbase) override;

  int shutdownDatabase(TRI_vocbase_t* vocbase) override;

  /// @brief Add engine-specific AQL functions.
  void addAqlFunctions() override;

  /// @brief Add engine-specific optimizer rules
  void addOptimizerRules() override;

  /// @brief Add engine-specific V8 functions
  void addV8Functions() override;

  /// @brief Add engine-specific REST handlers
  void addRestHandlers(rest::RestHandlerFactory*) override;

  void addParametersForNewCollection(arangodb::velocypack::Builder& builder,
                                     arangodb::velocypack::Slice info) override;
  void addParametersForNewIndex(arangodb::velocypack::Builder& builder,
                                arangodb::velocypack::Slice info) override;

  rocksdb::TransactionDB* db() const { return _db; }

  Result writeDatabaseMarker(TRI_voc_tick_t id, velocypack::Slice const& slice,
                             RocksDBLogValue&& logValue);
  int writeCreateCollectionMarker(TRI_voc_tick_t databaseId, TRI_voc_cid_t id,
                                  velocypack::Slice const& slice,
                                  RocksDBLogValue&& logValue);

  void addCollectionMapping(uint64_t, TRI_voc_tick_t, TRI_voc_cid_t);
  std::pair<TRI_voc_tick_t, TRI_voc_cid_t> mapObjectToCollection(
      uint64_t) const;

  std::vector<std::string> currentWalFiles();
  void determinePrunableWalFiles(TRI_voc_tick_t minTickToKeep);
  void pruneWalFiles();

 private:
  velocypack::Builder getReplicationApplierConfiguration(RocksDBKey const& key, int& status);
  int removeReplicationApplierConfiguration(RocksDBKey const& key);
  int saveReplicationApplierConfiguration(RocksDBKey const& key, arangodb::velocypack::Slice slice, bool doSync);
  Result dropDatabase(TRI_voc_tick_t);
  bool systemDatabaseExists();
  void addSystemDatabase();
  /// @brief open an existing database. internal function
  TRI_vocbase_t* openExistingDatabase(TRI_voc_tick_t id,
                                      std::string const& name,
                                      bool wasCleanShutdown, bool isUpgrade);

  std::string getCompressionSupport() const;

#ifdef USE_ENTERPRISE
  void collectEnterpriseOptions(std::shared_ptr<options::ProgramOptions>);
  void validateEnterpriseOptions(std::shared_ptr<options::ProgramOptions>);
  void prepareEnterprise();
  void startEnterprise();
  void configureEnterpriseRocksDBOptions(rocksdb::Options& options);

  enterprise::RocksDBEngineEEData _eeData;
#endif

 public:
  static std::string const EngineName;
  static std::string const FeatureName;
  
  /// @brief recovery manager
  RocksDBCounterManager* counterManager() const {
    TRI_ASSERT(_counterManager);
    return _counterManager.get();
  }
  
  /// @brief manages the ongoing dump clients
  RocksDBReplicationManager* replicationManager() const {
    TRI_ASSERT(_replicationManager);
    return _replicationManager.get();
  }

 private:
  /// single rocksdb database used in this storage engine
  rocksdb::TransactionDB* _db;
  /// default read options
  rocksdb::Options _options;
  /// arangodb comparator - requried because of vpack in keys
  std::unique_ptr<RocksDBVPackComparator> _vpackCmp;
  /// path used by rocksdb (inside _basePath)
  std::string _path;
  /// path to arangodb data dir
  std::string _basePath;
    
  /// @brief repository for replication contexts
  std::unique_ptr<RocksDBReplicationManager> _replicationManager;
  /// @brief tracks the count of documents in collections
  std::unique_ptr<RocksDBCounterManager> _counterManager;
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

  mutable basics::ReadWriteLock _collectionMapLock;
  std::unordered_map<uint64_t, std::pair<TRI_voc_tick_t, TRI_voc_cid_t>>
      _collectionMap;

  // which WAL files can be pruned when
  std::unordered_map<std::string, double> _prunableWalFiles;

  // number of seconds to wait before an obsolete WAL file is actually pruned
  double _pruneWaitTime;
};
}  // namespace arangodb
#endif
