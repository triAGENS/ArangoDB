////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Result.h"
#include "Futures/Future.h"
#include "IResearchLinkMock.h"
#include "Indexes/IndexIterator.h"
#include "Mocks/IResearchLinkMock.h"
#include "Mocks/IResearchInvertedIndexMock.h"
#include "Replication2/ReplicatedLog/PersistedLog.h"
#include "StorageEngine/HealthData.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/Identifiers/LocalDocumentId.h"

#include <atomic>
#include <string_view>

namespace arangodb {

class TransactionManager;
class WalAccess;

namespace aql {
class OptimizerRulesFeature;
}

namespace futures {
template<typename T>
class Future;
}

}  // namespace arangodb

class PhysicalCollectionMock : public arangodb::PhysicalCollection {
 public:
  struct DocElement {
    DocElement(std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> data,
               uint64_t docId);

    arangodb::velocypack::Slice data() const;
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> rawData() const;
    arangodb::LocalDocumentId docId() const;
    uint8_t const* vptr() const;
    void swapBuffer(
        std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>>& newData);

   private:
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> _data;
    uint64_t const _docId;
  };

  static std::function<void()> before;
  std::string physicalPath;

  PhysicalCollectionMock(arangodb::LogicalCollection& collection,
                         arangodb::velocypack::Slice const& info);
  virtual PhysicalCollection* clone(
      arangodb::LogicalCollection& collection) const override;
  virtual ErrorCode close() override;
  virtual std::shared_ptr<arangodb::Index> createIndex(
      arangodb::velocypack::Slice info, bool restore, bool& created) override;
  Result updateIndex(IndexId, velocypack::Slice) override {
    return {TRI_ERROR_NOT_IMPLEMENTED};
  }
  virtual void deferDropCollection(
      std::function<bool(arangodb::LogicalCollection&)> const& callback)
      override;
  virtual bool dropIndex(arangodb::IndexId iid) override;
  virtual void figuresSpecific(bool details,
                               arangodb::velocypack::Builder&) override;
  virtual std::unique_ptr<arangodb::IndexIterator> getAllIterator(
      arangodb::transaction::Methods* trx,
      arangodb::ReadOwnWrites readOwnWrites) const override;
  virtual std::unique_ptr<arangodb::IndexIterator> getAnyIterator(
      arangodb::transaction::Methods* trx) const override;
  virtual std::unique_ptr<arangodb::ReplicationIterator> getReplicationIterator(
      arangodb::ReplicationIterator::Ordering, uint64_t) override;
  virtual void getPropertiesVPack(
      arangodb::velocypack::Builder&) const override;
  virtual arangodb::Result insert(
      arangodb::transaction::Methods& trx, arangodb::RevisionId newRevisionId,
      arangodb::velocypack::Slice newDocument,
      arangodb::OperationOptions const& options) override;

  virtual arangodb::Result lookupKey(
      arangodb::transaction::Methods*, std::string_view,
      std::pair<arangodb::LocalDocumentId, arangodb::RevisionId>&,
      arangodb::ReadOwnWrites) const override;
  virtual uint64_t numberDocuments(
      arangodb::transaction::Methods* trx) const override;
  virtual std::string const& path() const override;
  virtual void prepareIndexes(
      arangodb::velocypack::Slice indexesSlice) override;

  arangodb::IndexEstMap clusterIndexEstimates(
      bool allowUpdating, arangodb::TransactionId tid) override;

  virtual arangodb::Result read(
      arangodb::transaction::Methods*, std::string_view key,
      arangodb::IndexIterator::DocumentCallback const& cb,
      arangodb::ReadOwnWrites) const override;
  virtual arangodb::Result read(
      arangodb::transaction::Methods* trx,
      arangodb::LocalDocumentId const& token,
      arangodb::IndexIterator::DocumentCallback const& cb,
      arangodb::ReadOwnWrites) const override;
  virtual arangodb::Result lookupDocument(
      arangodb::transaction::Methods& trx, arangodb::LocalDocumentId token,
      arangodb::velocypack::Builder& builder, bool readCache, bool fillCache,
      arangodb::ReadOwnWrites readOwnWrites) const override;
  virtual arangodb::Result remove(
      arangodb::transaction::Methods& trx,
      arangodb::LocalDocumentId previousDocumentId,
      arangodb::RevisionId previousRevisionId,
      arangodb::velocypack::Slice previousDocument,
      arangodb::OperationOptions const& options) override;
  virtual arangodb::Result replace(
      arangodb::transaction::Methods& trx,
      arangodb::LocalDocumentId newDocumentId,
      arangodb::RevisionId previousRevisionId,
      arangodb::velocypack::Slice previousDocument,
      arangodb::RevisionId newRevisionId,
      arangodb::velocypack::Slice newDocument,
      arangodb::OperationOptions const& options) override;
  virtual arangodb::RevisionId revision(
      arangodb::transaction::Methods* trx) const override;
  virtual arangodb::Result truncate(arangodb::transaction::Methods& trx,
                                    arangodb::OperationOptions& options,
                                    bool& usedRangeDelete) override;
  virtual void compact() override {}
  virtual arangodb::Result update(
      arangodb::transaction::Methods& trx,
      arangodb::LocalDocumentId newDocumentId,
      arangodb::RevisionId previousRevisionId,
      arangodb::velocypack::Slice previousDocument,
      arangodb::RevisionId newRevisionId,
      arangodb::velocypack::Slice newDocument,
      arangodb::OperationOptions const& options) override;
  virtual arangodb::Result updateProperties(
      arangodb::velocypack::Slice slice) override;

 private:
  bool addIndex(std::shared_ptr<arangodb::Index> idx);

  arangodb::Result updateInternal(arangodb::transaction::Methods& trx,
                                  arangodb::LocalDocumentId newDocumentId,
                                  arangodb::RevisionId previousRevisionId,
                                  arangodb::velocypack::Slice previousDocument,
                                  arangodb::RevisionId newRevisionId,
                                  arangodb::velocypack::Slice newDocument,
                                  arangodb::OperationOptions const& options,
                                  bool isUpdate);

  uint64_t _lastDocumentId;
  // map _key => data. Keyslice references memory in the value
  std::unordered_map<std::string_view, DocElement> _documents;
};

class TransactionCollectionMock : public arangodb::TransactionCollection {
 public:
  TransactionCollectionMock(arangodb::TransactionState* state,
                            arangodb::DataSourceId cid,
                            arangodb::AccessMode::Type accessType);
  virtual bool canAccess(arangodb::AccessMode::Type accessType) const override;
  virtual bool hasOperations() const override;
  virtual void releaseUsage() override;
  virtual arangodb::Result lockUsage() override;

 private:
  arangodb::Result doLock(arangodb::AccessMode::Type type) override;
  arangodb::Result doUnlock(arangodb::AccessMode::Type type) override;
};

class TransactionStateMock : public arangodb::TransactionState {
 public:
  static std::atomic_size_t abortTransactionCount;
  static std::atomic_size_t beginTransactionCount;
  static std::atomic_size_t commitTransactionCount;

  TransactionStateMock(TRI_vocbase_t& vocbase, arangodb::TransactionId tid,
                       arangodb::transaction::Options const& options);
  virtual arangodb::Result abortTransaction(
      arangodb::transaction::Methods* trx) override;
  virtual arangodb::Result beginTransaction(
      arangodb::transaction::Hints hints) override;
  virtual arangodb::futures::Future<arangodb::Result> commitTransaction(
      arangodb::transaction::Methods* trx) override;
  virtual arangodb::futures::Future<arangodb::Result>
  performIntermediateCommitIfRequired(arangodb::DataSourceId cid) override;
  uint64_t numPrimitiveOperations() const noexcept override { return 0; }
  virtual uint64_t numCommits() const noexcept override;
  virtual uint64_t numIntermediateCommits() const noexcept override;
  virtual void addIntermediateCommits(uint64_t value) override;
  virtual bool hasFailedOperations() const noexcept override;
  TRI_voc_tick_t lastOperationTick() const noexcept override;

  arangodb::Result triggerIntermediateCommit() override;
  std::unique_ptr<arangodb::TransactionCollection> createTransactionCollection(
      arangodb::DataSourceId cid,
      arangodb::AccessMode::Type accessType) override;
};

class StorageEngineMock : public arangodb::StorageEngine {
 public:
  static std::function<void()> before;
  static arangodb::Result flushSubscriptionResult;
  static arangodb::RecoveryState recoveryStateResult;
  static TRI_voc_tick_t recoveryTickResult;
  static std::string versionFilenameResult;
  static std::function<void()> recoveryTickCallback;
  std::map<std::pair<TRI_voc_tick_t, arangodb::DataSourceId>,
           arangodb::velocypack::Builder>
      views;
  std::atomic<size_t> vocbaseCount;

  explicit StorageEngineMock(arangodb::ArangodServer& server,
                             bool injectClusterIndexes = false);
  arangodb::HealthData healthCheck() override;
  virtual void addOptimizerRules(
      arangodb::aql::OptimizerRulesFeature& feature) override;
  virtual void addRestHandlers(
      arangodb::rest::RestHandlerFactory& handlerFactory) override;
  virtual void addV8Functions() override;
  virtual void changeCollection(
      TRI_vocbase_t& vocbase,
      arangodb::LogicalCollection const& collection) override;
  arangodb::Result changeView(arangodb::LogicalView const& view,
                              arangodb::velocypack::Slice update) override;

  virtual void createCollection(
      TRI_vocbase_t& vocbase,
      arangodb::LogicalCollection const& collection) override;
  virtual std::unique_ptr<TRI_vocbase_t> createDatabase(
      arangodb::CreateDatabaseInfo&&, ErrorCode& status) override;
  virtual arangodb::Result createLoggerState(TRI_vocbase_t*,
                                             VPackBuilder&) override;
  virtual std::unique_ptr<arangodb::PhysicalCollection>
  createPhysicalCollection(arangodb::LogicalCollection& collection,
                           arangodb::velocypack::Slice const& info) override;
  virtual arangodb::Result createTickRanges(VPackBuilder&) override;
  virtual std::unique_ptr<arangodb::transaction::Manager>
  createTransactionManager(arangodb::transaction::ManagerFeature&) override;
  virtual std::shared_ptr<arangodb::TransactionState> createTransactionState(
      TRI_vocbase_t& vocbase, arangodb::TransactionId tid,
      arangodb::transaction::Options const& options) override;
  virtual arangodb::Result createView(
      TRI_vocbase_t& vocbase, arangodb::DataSourceId id,
      arangodb::LogicalView const& view) override;
  virtual arangodb::Result compactAll(bool changeLevels,
                                      bool compactBottomMostLevel) override;
  virtual TRI_voc_tick_t currentTick() const override;
  virtual std::string dataPath() const override;
  virtual std::string databasePath(TRI_vocbase_t const* vocbase) const override;
  virtual arangodb::Result dropCollection(
      TRI_vocbase_t& vocbase, arangodb::LogicalCollection& collection) override;
  virtual arangodb::Result dropDatabase(TRI_vocbase_t& vocbase) override;
  virtual arangodb::Result dropView(TRI_vocbase_t const& vocbase,
                                    arangodb::LogicalView const& view) override;
  virtual arangodb::Result firstTick(uint64_t&) override;
  virtual std::vector<std::string> currentWalFiles() const override;
  virtual arangodb::Result flushWal(bool waitForSync,
                                    bool waitForCollector) override;
  virtual void getCollectionInfo(TRI_vocbase_t& vocbase,
                                 arangodb::DataSourceId cid,
                                 arangodb::velocypack::Builder& result,
                                 bool includeIndexes,
                                 TRI_voc_tick_t maxTick) override;
  virtual ErrorCode getCollectionsAndIndexes(
      TRI_vocbase_t& vocbase, arangodb::velocypack::Builder& result,
      bool wasCleanShutdown, bool isUpgrade) override;
  virtual void getDatabases(arangodb::velocypack::Builder& result) override;
  virtual void cleanupReplicationContexts() override;
  virtual arangodb::velocypack::Builder getReplicationApplierConfiguration(
      TRI_vocbase_t& vocbase, ErrorCode& result) override;
  virtual arangodb::velocypack::Builder getReplicationApplierConfiguration(
      ErrorCode& result) override;
  virtual ErrorCode getViews(TRI_vocbase_t& vocbase,
                             arangodb::velocypack::Builder& result) override;
  virtual arangodb::Result handleSyncKeys(
      arangodb::DatabaseInitialSyncer& syncer, arangodb::LogicalCollection& col,
      std::string const& keysId) override;
  virtual arangodb::RecoveryState recoveryState() override;
  virtual TRI_voc_tick_t recoveryTick() override;

  virtual arangodb::Result lastLogger(
      TRI_vocbase_t& vocbase, uint64_t tickStart, uint64_t tickEnd,
      arangodb::velocypack::Builder& builderSPtr) override;

  virtual std::unique_ptr<TRI_vocbase_t> openDatabase(
      arangodb::CreateDatabaseInfo&&, bool isUpgrade) override;
  using StorageEngine::registerCollection;
  using StorageEngine::registerView;
  virtual TRI_voc_tick_t releasedTick() const override;
  virtual void releaseTick(TRI_voc_tick_t) override;
  virtual ErrorCode removeReplicationApplierConfiguration(
      TRI_vocbase_t& vocbase) override;
  virtual ErrorCode removeReplicationApplierConfiguration() override;
  virtual arangodb::Result renameCollection(
      TRI_vocbase_t& vocbase, arangodb::LogicalCollection const& collection,
      std::string const& oldName) override;
  virtual ErrorCode saveReplicationApplierConfiguration(
      TRI_vocbase_t& vocbase, arangodb::velocypack::Slice slice,
      bool doSync) override;
  virtual ErrorCode saveReplicationApplierConfiguration(
      arangodb::velocypack::Slice, bool) override;
  virtual std::string versionFilename(TRI_voc_tick_t) const override;
  virtual void waitForEstimatorSync(
      std::chrono::milliseconds maxWaitTime) override;
  virtual arangodb::WalAccess const* walAccess() const override;

  static std::shared_ptr<arangodb::iresearch::IResearchLinkMock> buildLinkMock(
      arangodb::IndexId id, arangodb::LogicalCollection& collection,
      VPackSlice const& info);

  static std::shared_ptr<arangodb::iresearch::IResearchInvertedIndexMock>
  buildInvertedIndexMock(arangodb::IndexId id,
                         arangodb::LogicalCollection& collection,
                         VPackSlice const& info);

  auto createReplicatedLog(TRI_vocbase_t& vocbase,
                           arangodb::replication2::LogId id)
      -> arangodb::ResultT<std::shared_ptr<
          arangodb::replication2::replicated_log::PersistedLog>> override;
  auto dropReplicatedLog(
      TRI_vocbase_t& vocbase,
      std::shared_ptr<
          arangodb::replication2::replicated_log::PersistedLog> const& ptr)
      -> arangodb::Result override;

  auto updateReplicatedState(
      TRI_vocbase_t& vocbase,
      const arangodb::replication2::replicated_state::PersistedStateInfo& info)
      -> arangodb::Result override;
  auto dropReplicatedState(TRI_vocbase_t& vocbase,
                           arangodb::replication2::LogId id)
      -> arangodb::Result override;

 private:
  TRI_voc_tick_t _releasedTick;
};
