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

#include "RocksDBCollection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/ReadLocker.h"
#include "Basics/RecursiveLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/debugging.h"
#include "Basics/hashes.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/TransactionalCache.h"
#include "Cluster/ClusterMethods.h"
#include "Replication2/Version.h"
#include "IResearch/IResearchRocksDBInvertedIndex.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "Random/RandomGenerator.h"
#include "RestServer/DatabaseFeature.h"
#include "Logger/LogMacros.h"
#include "Metrics/Counter.h"
#include "Metrics/Histogram.h"
#include "Metrics/LogScale.h"
#include "Metrics/MetricsFeature.h"
#include "RocksDBEngine/RocksDBBuilderIndex.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBIndexingDisabler.h"
#include "RocksDBEngine/RocksDBIterators.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "RocksDBEngine/RocksDBReplicationContextGuard.h"
#include "RocksDBEngine/RocksDBReplicationIterator.h"
#include "RocksDBEngine/RocksDBReplicationManager.h"
#include "RocksDBEngine/RocksDBSavePoint.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBTransactionMethods.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Hints.h"
#include "Transaction/IndexesSnapshot.h"
#include "Utils/CollectionGuard.h"
#include "Utils/DatabaseGuard.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/Identifiers/RevisionId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/ticks.h"
#include "VocBase/voc-types.h"

#include <absl/strings/str_cat.h>
#include <Replication2/StateMachines/Document/DocumentLeaderState.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <velocypack/Iterator.h>

using namespace arangodb;

namespace {
using DocumentCacheType = cache::TransactionalCache<cache::BinaryKeyHasher>;

// number of write operations in transactions after which we will start
// doing preflight checks before every document insert or update/replace.
// the rationale is that if we already have a lot of operations accumulated
// in our transaction's WriteBatch, every rollback due to a unique constraint
// violation will be prohibitively expensive for larger WriteBatch sizes.
// so instead of performing an insert, update/replace directly, we first
// check for uniqueness violations and instantly abort if there are any,
// without having modified the transaction's WriteBatch for the failed
// operation. We can thus avoid the costly RollbackToSavePoint() call here.
// we don't do this preflight check for smaller batches though.
constexpr size_t preflightThreshold = 100;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
void validateNoExternals(velocypack::Slice value) {
  // recursively validate there the to-be-stored document
  // does not contain any VelocyPack Externals. this would be
  // invalid, because Externals are just pointers to memory,
  // and so they must never be persisted.
  TRI_ASSERT(!value.isExternal());
  if (value.isArray()) {
    for (VPackSlice it : VPackArrayIterator(value)) {
      validateNoExternals(it);
    }
  } else if (value.isObject()) {
    for (auto it : VPackObjectIterator(value, true)) {
      validateNoExternals(it.value);
    }
  }
}
#endif

// verify that the structure of a saved document is actually as expected
void verifyDocumentStructure(velocypack::Slice document,
                             bool isEdgeCollection) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(document.isObject());

  // _key, _id, _from, _to, _rev
  uint8_t const* p =
      document.begin() + document.findDataOffset(document.head());

  // _key
  TRI_ASSERT(*p == basics::VelocyPackHelper::KeyAttribute);
  ++p;
  TRI_ASSERT(VPackSlice(p).isString());
  p += VPackSlice(p).byteSize();

  // _id
  TRI_ASSERT(*p == basics::VelocyPackHelper::IdAttribute);
  ++p;
  TRI_ASSERT(VPackSlice(p).isCustom());
  p += VPackSlice(p).byteSize();

  if (isEdgeCollection) {
    // _from
    TRI_ASSERT(*p == basics::VelocyPackHelper::FromAttribute);
    ++p;
    TRI_ASSERT(VPackSlice(p).isString());
    p += VPackSlice(p).byteSize();

    // _to
    TRI_ASSERT(*p == basics::VelocyPackHelper::ToAttribute);
    ++p;
    TRI_ASSERT(VPackSlice(p).isString());
    p += VPackSlice(p).byteSize();
  }

  // _rev
  TRI_ASSERT(*p == basics::VelocyPackHelper::RevAttribute);
  ++p;
  TRI_ASSERT(VPackSlice(p).isString());

  validateNoExternals(document);
#endif
}

LocalDocumentId generateDocumentId(LogicalCollection const& collection,
                                   RevisionId revisionId) {
  bool useRev = collection.usesRevisionsAsDocumentIds();
  return useRev ? LocalDocumentId::create(revisionId)
                : LocalDocumentId::create();
}

template<typename F>
void reverseIdxOps(std::vector<std::shared_ptr<Index>> const& indexes,
                   std::vector<std::shared_ptr<Index>>::const_iterator& it,
                   F&& op) {
  while (it != indexes.begin()) {
    --it;
    auto& rIdx = basics::downCast<RocksDBIndex>(*it->get());
    if (rIdx.needsReversal()) {
      if (std::forward<F>(op)(rIdx).fail()) {
        // best effort for reverse failed. Let`s trigger full rollback
        // or we will end up with inconsistent storage and indexes
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "Failed to reverse index operation.");
      }
    }
  }
}

/// @brief remove an index from a container, by id
bool removeIndex(PhysicalCollection::IndexContainerType& indexes, IndexId id) {
  auto it = indexes.begin();
  while (it != indexes.end()) {
    if ((*it)->id() == id) {
      indexes.erase(it);
      return true;
    }
    ++it;
  }
  return false;
}

/// @brief helper RAII base class to count and time-track a CRUD operation
struct TimeTracker {
  TimeTracker(TimeTracker const&) = delete;
  TimeTracker& operator=(TimeTracker const&) = delete;

  using Metrics = TransactionStatistics::ReadWriteMetrics;
  using Count = void (*)(Metrics& metrics, float time) noexcept;

  explicit TimeTracker(std::optional<Metrics>& metrics, Count count) noexcept
      : metrics{metrics.has_value() ? &*metrics : nullptr}, count{count} {
    if (metrics) {
      // time measurement is not free. only do it if metrics are enabled
      start = std::chrono::steady_clock::now();
    }
  }

  ~TimeTracker() {
    if (metrics) {
      // metrics gathering is not free. only do it if metrics are enabled. Unit
      // is seconds here
      count(*metrics, std::chrono::duration<float>(
                          std::chrono::steady_clock::now() - start)
                          .count());
    }
  }

 private:
  Metrics* metrics;
  Count count;
  std::chrono::time_point<std::chrono::steady_clock> start;
};

/// @brief helper RAII class to count and time-track a CRUD read operation
using ReadTimeTracker = TimeTracker;

/// @brief helper RAII class to count and time-track CRUD write operations
struct WriteTimeTracker final : public TimeTracker {
  explicit WriteTimeTracker(std::optional<Metrics>& metrics,
                            OperationOptions const& options,
                            Count count) noexcept
      : TimeTracker{metrics, count} {
    if (metrics) {  // metrics collection is not free. only track writes if
                    // metrics are enabled
      if (options.isSynchronousReplicationFrom.empty()) {
        metrics->numWrites.count();
      } else {
        metrics->numWritesReplication.count();
      }
    }
  }
};

/// @brief helper RAII class to count and time-track truncate operations
struct TruncateTimeTracker final : public TimeTracker {
  explicit TruncateTimeTracker(std::optional<Metrics>& metrics,
                               OperationOptions const& options,
                               Count count) noexcept
      : TimeTracker{metrics, count} {
    if (metrics) {  // metrics collection is not free. only track truncates if
                    // metrics are enabled
      if (options.isSynchronousReplicationFrom.empty()) {
        metrics->numTruncates.count();
      } else {
        metrics->numTruncatesReplication.count();
      }
    }
  }
};

[[maybe_unused]] void reportPrimaryIndexInconsistencyIfNotFound(
    Result const& res, LogicalCollection const& collection,
    std::string_view key, LocalDocumentId rev, ReadOwnWrites readOwnWrites,
    RocksDBTransactionState const* state) {
  TRI_ASSERT(res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND));
  // Scandal! A primary index entry is pointing to nowhere! Let's report
  // this to the authorities immediately:
  LOG_TOPIC("42536", ERR, arangodb::Logger::ENGINES)
      << "Found primary index entry for which there is no actual "
         "document: "
      << res.errorMessage() << ", collection: " << collection.vocbase().name()
      << "/" << collection.name() << ", type: "
      << (collection.type() == TRI_COL_TYPE_EDGE ? "edge" : "document")
      << ", smart: " << (collection.isSmart() ? "yes" : "no")
      << ", _key=" << key << ", _rev=" << RevisionId(rev).toHLC() << " ("
      << rev.id() << "), read own writes: "
      << (readOwnWrites == ReadOwnWrites::yes ? "yes" : "no")
      << ", state: " << state->debugInfo()
      << ", hlc: " << TRI_HybridLogicalClock();
  TRI_ASSERT(false);
}

size_t getParallelism(velocypack::Slice slice) {
  return basics::VelocyPackHelper::getNumericValue(
      slice, StaticStrings::IndexParallelism,
      IndexFactory::kDefaultParallelism);
}

}  // namespace

namespace arangodb {

// from IResearchKludge
void syncIndexOnCreate(Index&);

RocksDBCollection::RocksDBCollection(LogicalCollection& collection,
                                     velocypack::Slice info)
    : RocksDBMetaCollection(collection, info),
      _primaryIndex(nullptr),
      _cacheManager(collection.vocbase()
                        .server()
                        .getFeature<CacheManagerFeature>()
                        .manager()),
      _maxCacheValueSize(
          _cacheManager == nullptr ? 0 : _cacheManager->maxCacheValueSize()),
      _statistics(collection.vocbase()
                      .server()
                      .getFeature<metrics::MetricsFeature>()
                      .serverStatistics()
                      ._transactionsStatistics),
      _cacheEnabled(_cacheManager != nullptr && !collection.system() &&
                    !collection.isAStub() &&
                    !ServerState::instance()->isCoordinator() &&
                    basics::VelocyPackHelper::getBooleanValue(
                        info, StaticStrings::CacheEnabled, false)) {
  TRI_ASSERT(_logicalCollection.isAStub() || objectId() != 0);
  if (_cacheEnabled.load(std::memory_order_relaxed)) {
    setupCache();
  }
}

RocksDBCollection::~RocksDBCollection() {
  if (useCache()) {
    try {
      destroyCache();
    } catch (...) {
    }
  }
}

void RocksDBCollection::deferDropCollection(
    std::function<bool(LogicalCollection&)> const& cb) {
  RocksDBMetaCollection::deferDropCollection(cb);
  freeMemory();
}

void RocksDBCollection::freeMemory() noexcept {
  if (useCache()) {
    try {
      destroyCache();
    } catch (...) {
    }
  }

  {
    // remove all indexes from the collection object.
    // we do this because the objects of deleted collections will be
    // retained in memory until the server is shut down.
    // deleting the collection's attached index objects saves
    // memory for caches etc.
    // when we get here, the code in RocksDBEngine::dropCollection()
    // will have already called `drop()` on each index object.
    RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
    auto it = _indexes.begin();
    while (it != _indexes.end()) {
      auto const& idx = (*it);
      // unloading drops any potential caches
      idx->unload();

      if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
        // we keep the primary index object around, because it can
        // be referred to by the collection object with a pointer.
        ++it;
      } else {
        // all other index types we simply delete, so that the
        // underlying objects will be garbage-collected and memory
        // be freed.
        it = _indexes.erase(it);
      }
    }
  }

  RocksDBMetaCollection::freeMemory();

  auto& engine = _logicalCollection.vocbase().engine<RocksDBEngine>();
  engine.removeCollectionMapping(objectId());
}

Result RocksDBCollection::updateProperties(velocypack::Slice slice) {
  bool cacheEnabled = _cacheManager != nullptr &&
                      !_logicalCollection.system() &&
                      !_logicalCollection.isAStub() &&
                      !ServerState::instance()->isCoordinator() &&
                      basics::VelocyPackHelper::getBooleanValue(
                          slice, StaticStrings::CacheEnabled,
                          _cacheEnabled.load(std::memory_order_relaxed));
  _cacheEnabled.store(cacheEnabled, std::memory_order_relaxed);
  primaryIndex()->setCacheEnabled(cacheEnabled);

  if (cacheEnabled) {
    setupCache();
    primaryIndex()->setupCache();
  } else {
    // will do nothing if cache is not present
    destroyCache();
    primaryIndex()->destroyCache();
  }

  // nothing else to do
  return {};
}

/// @brief export properties
void RocksDBCollection::getPropertiesVPack(velocypack::Builder& result) const {
  TRI_ASSERT(result.isOpenObject());
  result.add(StaticStrings::ObjectId, VPackValue(std::to_string(objectId())));
  result.add(StaticStrings::CacheEnabled,
             VPackValue(_cacheEnabled.load(std::memory_order_relaxed)));
  TRI_ASSERT(result.isOpenObject());
}

/// return bounds for all documents
RocksDBKeyBounds RocksDBCollection::bounds() const {
  return RocksDBKeyBounds::CollectionDocuments(objectId());
}

// callback that is called while adding a new index. called under
// indexes write-lock
void RocksDBCollection::duringAddIndex(std::shared_ptr<Index> idx) {
  // update tick value and _primaryIndex member
  TRI_ASSERT(idx != nullptr);
  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(idx->id().id()));
  if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
    TRI_ASSERT(idx->id().isPrimary());
    _primaryIndex = static_cast<RocksDBPrimaryIndex*>(idx.get());
  }
}

futures::Future<std::shared_ptr<Index>> RocksDBCollection::createIndex(
    VPackSlice info, bool restore, bool& created,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress,
    replication2::replicated_state::document::Replication2Callback
        replicationCb) {
  TRI_ASSERT(info.isObject());

  // Step 0. Lock all the things
  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  auto& engine = vocbase.engine<RocksDBEngine>();

  DatabaseGuard dbGuard(vocbase);
  CollectionGuard guard(&vocbase, _logicalCollection.id());

  RocksDBBuilderIndex::Locker locker(this);
  if (!co_await locker.lock()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_LOCK_TIMEOUT);
  }

  {
    // Step 1. Check for existing matching index
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    if (auto existingIdx = findIndex(info, _indexes); existingIdx != nullptr) {
      // We already have this index.
      if (existingIdx->type() == arangodb::Index::TRI_IDX_TYPE_TTL_INDEX) {
        // special handling for TTL indexes
        // if there is exactly the same index present, we return it
        if (!existingIdx->matchesDefinition(info)) {
          // if there is another TTL index already, we make things abort here
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_BAD_PARAMETER,
              "there can only be one ttl index per collection");
        }
      }
      // same index already exists. return it
      created = false;
      co_return existingIdx;
    }

    auto const id = helpers::extractId(info);
    auto const name = helpers::extractName(info);

    // check all existing indexes for id/new conflicts
    for (auto const& other : _indexes) {
      if (other->id() == id || other->name() == name) {
        // definition shares an identifier with an existing index with a
        // different definition
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        VPackBuilder builder;
        other->toVelocyPack(
            builder, static_cast<std::underlying_type<Index::Serialize>::type>(
                         Index::Serialize::Basics));
        LOG_TOPIC("29d1c", WARN, Logger::ENGINES)
            << "attempted to create index '" << info.toJson()
            << "' but found conflicting index '" << builder.slice().toJson()
            << "'";
#endif
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
                                       "duplicate value for `" +
                                           StaticStrings::IndexId + "` or `" +
                                           StaticStrings::IndexName + "`");
      }
    }
  }

  // TODO(MBkkt) it's probably needed here on step 2 before step 5,
  //  because arangosearch links connected with views in prepareIndexFromSlice
  READ_LOCKER(inventoryLocker, vocbase._inventoryLock);

  // Step 2. Create new index object
  std::shared_ptr<Index> newIdx;
  try {
    newIdx = engine.indexFactory().prepareIndexFromSlice(
        info, /*generateKey*/ !restore, _logicalCollection, false);
  } catch (std::exception const& ex) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_CREATION_FAILED,
                                   ex.what());
  }

  // we cannot persist primary or edge indexes
  TRI_ASSERT(newIdx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  TRI_ASSERT(newIdx->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX);

  // cleanup newly instantiated object
  auto indexCleanup = ScopeGuard([&newIdx]() noexcept {
    try {
      newIdx->drop();
    } catch (...) {
      TRI_ASSERT(false);
    }
  });

  // until here we have been completely read only.
  // modifications start now...
  auto lambda = [&]() -> futures::Future<Result> {
    Result res;

    // Step 3. add index to collection entry (for removal after a crash)
    auto buildIdx = std::make_shared<RocksDBBuilderIndex>(
        std::static_pointer_cast<RocksDBIndex>(newIdx), _meta.numberDocuments(),
        getParallelism(info));
    if (!engine.inRecovery()) {
      // manually modify collection entry, other methods need lock
      RocksDBKey key;  // read collection info from database
      key.constructCollection(vocbase.id(), _logicalCollection.id());
      rocksdb::PinnableSlice ps;
      rocksdb::Status s =
          engine.db()->Get(rocksdb::ReadOptions(),
                           RocksDBColumnFamilyManager::get(
                               RocksDBColumnFamilyManager::Family::Definitions),
                           key.string(), &ps);
      if (!s.ok()) {
        co_return res.reset(rocksutils::convertStatus(s));
      }

      VPackBuilder builder;
      builder.openObject();
      for (auto pair : VPackObjectIterator(RocksDBValue::data(ps))) {
        if (pair.key.isEqualString("indexes")) {  // append new index
          VPackArrayBuilder arrGuard(&builder, "indexes");
          builder.add(VPackArrayIterator(pair.value));
          buildIdx->toVelocyPack(builder,
                                 Index::makeFlags(Index::Serialize::Internals));
        } else {
          builder.add(pair.key);
          builder.add(pair.value);
        }
      }
      builder.close();
      res = engine.writeCreateCollectionMarker(
          vocbase.id(), _logicalCollection.id(), builder.slice(),
          RocksDBLogValue::Empty());
      if (res.fail()) {
        co_return res;
      }
    }

    // release inventory lock while we are filling the index
    inventoryLocker.unlock();

    // Step 4. fill index
    bool const inBackground = basics::VelocyPackHelper::getBooleanValue(
        info, StaticStrings::IndexInBackground, false);

    if (inBackground) {
      // allow concurrent inserts into index
      {
        RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
        _indexes.emplace(buildIdx);
      }

      RocksDBFilePurgePreventer walKeeper(&engine);
      res = co_await buildIdx->fillIndexBackground(locker, std::move(progress));
    } else {
      res = buildIdx->fillIndexForeground(std::move(progress));
    }
    if (res.fail()) {
      co_return res;
    }

    ADB_PROD_ASSERT(locker.isLocked())
        << "Internal error: lock already gone during index creation";

    syncIndexOnCreate(*newIdx);

    inventoryLocker.lock();

    // step 4½. replicate index creation
    if (vocbase.replicationVersion() == replication::Version::TWO &&
        replicationCb != nullptr) {
      // On a leader, we need to replicate the create index operation now;
      // on both leader and follower, we need to persist information to make
      // unique indexes safe to replay. This happens during the callback.
      // In case anything goes wrong here, the index must be dropped.
      auto replicationResult = co_await replicationCb(info);
      if (!replicationResult.ok()) {
        // abort index creation
        THROW_ARANGO_EXCEPTION(replicationResult);
      }
    }

    // Step 5. register in index list
    {
      RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
      if (inBackground) {
        // remove temporary index and swap in actual index
        removeIndex(_indexes, buildIdx->id());
      }
      _indexes.emplace(newIdx);
    }

    // inBackground index might not recover selectivity estimate w/o sync
    if (inBackground && !newIdx->unique() && newIdx->hasSelectivityEstimate()) {
      engine.settingsManager()->sync(/*force*/ false);
    }

    // Step 6. persist in rocksdb
    if (!engine.inRecovery()) {
      // write new collection marker
      auto builder = _logicalCollection.toVelocyPackIgnore(
          {"path", "statusString"},
          LogicalDataSource::Serialization::PersistenceWithInProgress);
      VPackBuilder indexInfo;
      newIdx->toVelocyPack(indexInfo,
                           Index::makeFlags(Index::Serialize::Internals));
      res = engine.writeCreateCollectionMarker(
          vocbase.id(), _logicalCollection.id(), builder.slice(),
          RocksDBLogValue::IndexCreate(vocbase.id(), _logicalCollection.id(),
                                       indexInfo.slice()));
    }

    co_return res;
  };
  Result res = co_await asResult(lambda());

  if (res.ok()) {
    created = true;
    indexCleanup.cancel();
    co_return newIdx;
  }

  // cleanup routine
  // We could not create the index. Better abort
  {
    RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
    removeIndex(_indexes, newIdx->id());
  }
  THROW_ARANGO_EXCEPTION(res);
}

// callback that is called directly before the index is dropped.
// the write-lock on all indexes is still held. this is not called
// during recovery.
Result RocksDBCollection::duringDropIndex(std::shared_ptr<Index> idx) {
  auto& engine = _logicalCollection.vocbase().engine<RocksDBEngine>();
  TRI_ASSERT(!engine.inRecovery());

  auto builder = _logicalCollection.toVelocyPackIgnore(
      {"path", "statusString"},
      LogicalDataSource::Serialization::PersistenceWithInProgress);
  // log this event in the WAL and in the collection meta-data
  return engine.writeCreateCollectionMarker(  // write marker
      _logicalCollection.vocbase().id(),      // vocbase id
      _logicalCollection.id(),                // collection id
      builder.slice(),                        // RocksDB path
      RocksDBLogValue::IndexDrop(             // marker
          _logicalCollection.vocbase().id(), _logicalCollection.id(),
          idx->id()  // args
          ));
}

// callback that is called directly after the index has been dropped.
// no locks are held anymore.
Result RocksDBCollection::afterDropIndex(std::shared_ptr<Index> idx) {
  TRI_ASSERT(idx != nullptr);

  auto cindex = std::static_pointer_cast<RocksDBIndex>(idx);
  Result res = cindex->drop();

  if (res.ok() && meta().numberDocuments() >= 32 * 1024) {
    cindex->compact();  // trigger compaction to reclaim disk space
  }

  return res;
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAllIterator(
    transaction::Methods* trx, ReadOwnWrites readOwnWrites) const {
  return rocksdb_iterators::createAllIterator(&_logicalCollection, trx,
                                              readOwnWrites);
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAnyIterator(
    transaction::Methods* trx) const {
  return rocksdb_iterators::createAnyIterator(&_logicalCollection, trx);
}

std::unique_ptr<ReplicationIterator> RocksDBCollection::getReplicationIterator(
    ReplicationIterator::Ordering order, uint64_t batchId) {
  if (order != ReplicationIterator::Ordering::Revision) {
    // not supported
    return nullptr;
  }

  if (batchId != 0) {
    RocksDBReplicationManager* manager = _logicalCollection.vocbase()
                                             .engine<RocksDBEngine>()
                                             .replicationManager();

    RocksDBReplicationContextGuard ctx = manager->find(batchId);
    if (ctx) {
      return std::make_unique<RocksDBRevisionReplicationIterator>(
          _logicalCollection, ctx->snapshot());
    }
    // fallthrough intentional
  }

  return std::make_unique<RocksDBRevisionReplicationIterator>(
      _logicalCollection, /*snapshot*/ nullptr);
}

std::unique_ptr<ReplicationIterator> RocksDBCollection::getReplicationIterator(
    ReplicationIterator::Ordering order, transaction::Methods& trx) {
  if (order != ReplicationIterator::Ordering::Revision) {
    // not supported
    return nullptr;
  }

  return std::make_unique<RocksDBRevisionReplicationIterator>(
      _logicalCollection, trx);
}

////////////////////////////////////
// -- SECTION DML Operations --
///////////////////////////////////

Result RocksDBCollection::truncate(transaction::Methods& trx,
                                   OperationOptions& options,
                                   bool& usedRangeDelete) {
  ::TruncateTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_truncate_sec.count(time); });

  auto state = RocksDBTransactionState::toState(&trx);
  TRI_ASSERT(!state->isReadOnlyTransaction());

  if (state->isOnlyExclusiveTransaction() &&
      state->hasHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE) &&
      _meta.numberDocuments() >= 32 * 1024) {
    // optimized truncate, using DeleteRange operations.
    // this can only be used if the truncate is performed as a standalone
    // operation (i.e. not part of a larger transaction)
    usedRangeDelete = true;
    return truncateWithRangeDelete(trx);
  }

  // slow truncate that performs a document-by-document removal.
  usedRangeDelete = false;
  return truncateWithRemovals(trx, options);
}

Result RocksDBCollection::truncateWithRangeDelete(transaction::Methods& trx) {
  // non-transactional truncate optimization. We perform a bunch of
  // range deletes and circumvent the normal rocksdb::Transaction.
  // no savepoint needed here
  auto state = RocksDBTransactionState::toState(&trx);
  TRI_ASSERT(!state->hasOperations());  // not allowed

  TRI_ASSERT(objectId() != 0);

  TRI_IF_FAILURE("RocksDBRemoveLargeRangeOn") {
    return Result(TRI_ERROR_DEBUG);
  }

  auto& engine = _logicalCollection.vocbase().engine<RocksDBEngine>();
  rocksdb::DB* db = engine.db()->GetRootDB();

  TRI_IF_FAILURE("RocksDBCollection::truncate::forceSync") {
    engine.settingsManager()->sync(/*force*/ false);
  }

  // pre commit sequence needed to place a blocker
  RocksDBBlockerGuard blocker(&_logicalCollection);
  blocker.placeBlocker(state->id());

  rocksdb::WriteBatch batch;
  // delete documents
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(objectId());
  rocksdb::Status s =
      batch.DeleteRange(bounds.columnFamily(), bounds.start(), bounds.end());
  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  auto indexesSnapshot = getIndexesSnapshot();
  auto const& indexes = indexesSnapshot.getIndexes();

  // delete index values
  std::vector<TruncateGuard> guards;
  guards.reserve(indexes.size());
  for (auto const& idx : indexes) {
    auto* rIdx = basics::downCast<RocksDBIndex>(idx.get());
    auto r = rIdx->truncateBegin(batch);
    if (!r.ok()) {
      return std::move(r).result();
    }
    guards.push_back(std::move(r.get()));
  }

  // add the log entry so we can recover the correct count
  auto log = RocksDBLogValue::CollectionTruncate(
      trx.vocbase().id(), _logicalCollection.id(), objectId());

  s = batch.PutLogData(log.slice());
  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  s = db->Write(rocksdb::WriteOptions(), &batch);

  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }
  // we need crash server if any failure in order to trigger recovery,
  // as rocksdb truncate already committed here
  [&]() noexcept {
    // post commit sequence
    rocksdb::SequenceNumber seq = db->GetLatestSequenceNumber() - 1;

    uint64_t numDocs = _meta.numberDocuments();
    _meta.adjustNumberDocuments(seq,
                                /*revision*/ _logicalCollection.newRevisionId(),
                                -static_cast<int64_t>(numDocs));

    for (auto it = guards.begin(); auto const& idx : indexes) {
      // clears caches / clears links (if applicable)
      auto* rIdx = basics::downCast<RocksDBIndex>(idx.get());
      rIdx->truncateCommit(std::move(*it++), seq, &trx);
    }

    indexesSnapshot.release();

    bufferTruncate(seq);

    TRI_ASSERT(!state->hasOperations());  // not allowed
  }();
  return {};
}

Result RocksDBCollection::truncateWithRemovals(transaction::Methods& trx,
                                               OperationOptions& options) {
  TRI_IF_FAILURE("RocksDBRemoveLargeRangeOff") { return {TRI_ERROR_DEBUG}; }

  TRI_ASSERT(objectId() != 0);

  RocksDBKeyBounds documentBounds =
      RocksDBKeyBounds::CollectionDocuments(objectId());
  rocksdb::Comparator const* cmp =
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Documents)
          ->GetComparator();
  rocksdb::Slice end = documentBounds.end();

  // avoid OOM error for truncate by committing earlier
  auto state = RocksDBTransactionState::toState(&trx);
  uint64_t const prvICC = state->options().intermediateCommitCount;
  if (!state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    state->options().intermediateCommitCount =
        std::min<uint64_t>(prvICC, 10000);
  }

  // push our current transaction on the stack
  state->beginQuery(/*resourceMonitor*/ nullptr, /*isModificationQuery*/ true);
  auto stateGuard = scopeGuard([state, prvICC]() noexcept {
    state->endQuery(/*isModificationQuery*/ true);
    // reset to previous value after truncate is finished
    state->options().intermediateCommitCount = prvICC;
  });

  RocksDBTransactionMethods* mthds =
      state->rocksdbMethods(_logicalCollection.id());

  VPackBuilder keyBuffer;
  keyBuffer.openArray();
  uint64_t found = 0;

  auto removeBufferedDocuments = [this, &trx, &options](VPackBuilder& keyBuffer,
                                                        uint64_t& found) {
    TRI_ASSERT(found > 0);
    keyBuffer.close();

    TRI_ASSERT(keyBuffer.slice().isArray());
    TRI_ASSERT(keyBuffer.slice().length() > 0);

    // if waitForSync flag is set, update it for transaction and options
    if (_logicalCollection.waitForSync() && !options.isRestore) {
      options.waitForSync = true;
    }

    if (options.waitForSync) {
      trx.state()->waitForSync(true);
    }

    OperationResult r =
        trx.remove(_logicalCollection.name(), keyBuffer.slice(), options);

    // reset everything
    keyBuffer.clear();
    keyBuffer.openArray();

    found = 0;

    if (!r.countErrorCodes.empty()) {
      auto it = r.countErrorCodes.begin();
      return Result((*it).first);
    }

    return r.result;
  };

  constexpr bool readOwnWrites = false;
  auto iter =
      mthds->NewIterator(documentBounds.columnFamily(), [&](ReadOptions& ro) {
        if (!mthds->iteratorMustCheckBounds(
                readOwnWrites ? ReadOwnWrites::yes : ReadOwnWrites::no)) {
          ro.iterate_upper_bound = &end;
        }
        // we are going to blow away all data anyway. no need to blow up the
        // cache
        ro.fill_cache = false;
        ro.readOwnWrites = readOwnWrites;
        TRI_ASSERT(ro.snapshot);
      });
  for (iter->Seek(documentBounds.start());
       iter->Valid() && cmp->Compare(iter->key(), end) < 0; iter->Next()) {
    TRI_ASSERT(objectId() == RocksDBKey::objectId(iter->key()));
    VPackSlice document(reinterpret_cast<uint8_t const*>(iter->value().data()));
    TRI_ASSERT(document.isObject());

    // add key of to-be-deleted document
    VPackSlice key = document.get(StaticStrings::KeyString);
    TRI_ASSERT(key.isString());
    keyBuffer.add(key);

    ++found;
    if (found == 1000) {
      if (Result res = removeBufferedDocuments(keyBuffer, found); res.fail()) {
        return res;
      }
      TRI_ASSERT(found == 0);
    }
  }

  if (found > 0) {
    if (Result res = removeBufferedDocuments(keyBuffer, found); res.fail()) {
      return res;
    }
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (state->numCommits() == 0) {
    // check IN TRANSACTION if documents have been deleted
    if (mthds->countInBounds(RocksDBKeyBounds::CollectionDocuments(objectId()),
                             true)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "deletion check in collection truncate "
                                     "failed - not all documents have been "
                                     "deleted");
    }
  }
#endif

  TRI_IF_FAILURE("FailAfterAllCommits") { return Result(TRI_ERROR_DEBUG); }
  TRI_IF_FAILURE("SegfaultAfterAllCommits") {
    TRI_TerminateDebugging("SegfaultAfterAllCommits");
  }
  return {};
}

Result RocksDBCollection::lookupKey(
    transaction::Methods* trx, std::string_view key,
    std::pair<LocalDocumentId, RevisionId>& result,
    ReadOwnWrites readOwnWrites) const {
  return doLookupKey(trx, key, result, readOwnWrites, false);
}

Result RocksDBCollection::lookupKeyForUpdate(
    transaction::Methods* trx, std::string_view key,
    std::pair<LocalDocumentId, RevisionId>& result) const {
  return doLookupKey(trx, key, result, ReadOwnWrites::yes, true);
}

Result RocksDBCollection::doLookupKey(
    transaction::Methods* trx, std::string_view key,
    std::pair<LocalDocumentId, RevisionId>& result, ReadOwnWrites readOwnWrites,
    bool lockForUpdate) const {
  result.first = LocalDocumentId::none();
  result.second = RevisionId::none();

  // lookup the revision id in the primary index
  auto res = primaryIndex()->lookupRevision(
      trx, key, result.first, result.second, readOwnWrites, lockForUpdate);

  if (res.ok()) {
    TRI_ASSERT(result.first.isSet());
    TRI_ASSERT(result.second.isSet());
  } else {
    TRI_ASSERT(!result.first.isSet());
    TRI_ASSERT(result.second.empty());
  }
  return res;
}

bool RocksDBCollection::lookupRevision(transaction::Methods* trx,
                                       velocypack::Slice key,
                                       RevisionId& revisionId,
                                       ReadOwnWrites readOwnWrites) const {
  TRI_ASSERT(key.isString());
  std::pair<LocalDocumentId, RevisionId> v;
  auto res = lookupKey(trx, key.stringView(), v, readOwnWrites);
  if (res.ok()) {
    revisionId = v.second;
    return true;
  }
  return false;
}

Result RocksDBCollection::lookup(transaction::Methods* trx,
                                 std::string_view key,
                                 IndexIterator::DocumentCallback const& cb,
                                 LookupOptions options) const {
  TRI_IF_FAILURE("LogicalCollection::read") { return Result(TRI_ERROR_DEBUG); }

  ::ReadTimeTracker timeTracker(
      _statistics._readWriteMetrics,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_read_sec.count(time); });

  rocksdb::PinnableSlice ps;
  Result res;
  LocalDocumentId documentId;
  do {
    [[maybe_unused]] bool foundInCache;
    documentId = primaryIndex()->lookupKey(
        trx, key, static_cast<ReadOwnWrites>(options.readOwnWrites),
        foundInCache);
    if (!documentId.isSet()) {
      res.reset(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
      return res;
    }  // else found

    TRI_IF_FAILURE("RocksDBCollection::read-delay") {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(RandomGenerator::interval(uint32_t(2000))));
    }

    res = lookupDocumentVPack(trx, documentId, cb, options, nullptr);
  } while (res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
           RocksDBTransactionState::toState(trx)->ensureSnapshot());
  if (res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
    ::reportPrimaryIndexInconsistencyIfNotFound(
        res, _logicalCollection, key, documentId,
        static_cast<ReadOwnWrites>(options.readOwnWrites),
        RocksDBTransactionState::toState(trx));
  }
  if (options.countBytes) {
    trx->state()->trackShardUsage(
        *trx->resolver(), _logicalCollection.vocbase().name(),
        _logicalCollection.name(), trx->username(), AccessMode::Type::READ,
        "document read", ps.size());
  }
  return res;
}

Result RocksDBCollection::lookup(transaction::Methods* trx,
                                 LocalDocumentId token,
                                 IndexIterator::DocumentCallback const& cb,
                                 LookupOptions options,
                                 StorageSnapshot const* snapshot) const {
  ::ReadTimeTracker timeTracker(
      _statistics._readWriteMetrics,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_read_sec.count(time); });

  if (!token.isSet()) {
    return Result{TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD,
                  "invalid local document id"};
  }

  return lookupDocumentVPack(trx, token, cb, options, snapshot);
}

Result RocksDBCollection::lookup(transaction::Methods* trx,
                                 std::span<LocalDocumentId> tokens,
                                 MultiDocumentCallback const& cb,
                                 LookupOptions options) const {
  ::ReadTimeTracker timeTracker(
      _statistics._readWriteMetrics,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_read_sec.count(time); });

  RocksDBMethods* mthd =
      RocksDBTransactionState::toMethods(trx, _logicalCollection.id());
  auto* family = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Documents);

  constexpr auto keySize = 2 * sizeof(uint64_t);
  std::string buffer;
  buffer.resize(tokens.size() * keySize);

  std::vector<rocksdb::Slice> keys;
  keys.reserve(tokens.size());

  RocksDBKey key;

  for (std::size_t k = 0; k < tokens.size(); k++) {
    auto docId = tokens[k];
    key.constructDocument(objectId(), docId);
    auto start = buffer.begin() + k * keySize;
    std::copy(key.buffer()->begin(), key.buffer()->end(), start);
    auto slice = rocksdb::Slice(std::string_view{start, start + keySize});
    keys.push_back(slice);
  }

  std::vector<rocksdb::PinnableSlice> values;
  values.resize(tokens.size());
  std::vector<rocksdb::Status> statuses;
  statuses.resize(tokens.size());
  mthd->MultiGet(
      *family, tokens.size(), keys.data(), values.data(), statuses.data(),
      options.readOwnWrites ? ReadOwnWrites::yes : ReadOwnWrites::no);

  size_t totalSize = 0;

  for (std::size_t k = 0; k < tokens.size(); k++) {
    if (!statuses[k].ok()) {
      cb(rocksutils::convertStatus(statuses[k]), tokens[k], nullptr,
         VPackSlice::noneSlice());
    } else {
      // TODO optimization for non-pinned slices
      // for that we have leased strings for each value.
      cb(Result{}, tokens[k], nullptr,
         VPackSlice{reinterpret_cast<uint8_t const*>(values[k].data())});

      totalSize += values[k].size();
    }
  }

  if (options.countBytes) {
    trx->state()->trackShardUsage(
        *trx->resolver(), _logicalCollection.vocbase().name(),
        _logicalCollection.name(), trx->username(), AccessMode::Type::READ,
        "document multiget", totalSize);
  }

  return {};
}

Result RocksDBCollection::insert(transaction::Methods& trx,
                                 IndexesSnapshot const& indexesSnapshot,
                                 RevisionId newRevisionId,
                                 velocypack::Slice newDocument,
                                 OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_insert_sec.count(time); });

  TRI_ASSERT(newRevisionId.isSet());
  TRI_ASSERT(newDocument.isObject());
  ::verifyDocumentStructure(newDocument,
                            _logicalCollection.type() == TRI_COL_TYPE_EDGE);

  auto* state = RocksDBTransactionState::toState(&trx);
  RocksDBTransactionStateGuard transactionStateGuard(state);

  TRI_ASSERT(!state->isReadOnlyTransaction());

  LocalDocumentId newDocumentId =
      ::generateDocumentId(_logicalCollection, newRevisionId);

  RocksDBSavePoint savepoint(_logicalCollection.id(), *state,
                             TRI_VOC_DOCUMENT_OPERATION_INSERT);

  return insertDocument(&trx, indexesSnapshot, savepoint, newDocumentId,
                        newDocument, options, newRevisionId);
}

Result RocksDBCollection::update(
    transaction::Methods& trx, IndexesSnapshot const& indexesSnapshot,
    LocalDocumentId previousDocumentId, RevisionId previousRevisionId,
    velocypack::Slice previousDocument, RevisionId newRevisionId,
    velocypack::Slice newDocument, OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_update_sec.count(time); });

  return performUpdateOrReplace(trx, indexesSnapshot, previousDocumentId,
                                previousRevisionId, previousDocument,
                                newRevisionId, newDocument, options,
                                TRI_VOC_DOCUMENT_OPERATION_UPDATE);
}

Result RocksDBCollection::replace(
    transaction::Methods& trx, IndexesSnapshot const& indexesSnapshot,
    LocalDocumentId previousDocumentId, RevisionId previousRevisionId,
    velocypack::Slice previousDocument, RevisionId newRevisionId,
    velocypack::Slice newDocument, OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_replace_sec.count(time); });

  return performUpdateOrReplace(trx, indexesSnapshot, previousDocumentId,
                                previousRevisionId, previousDocument,
                                newRevisionId, newDocument, options,
                                TRI_VOC_DOCUMENT_OPERATION_REPLACE);
}

Result RocksDBCollection::performUpdateOrReplace(
    transaction::Methods& trx, IndexesSnapshot const& indexesSnapshot,
    LocalDocumentId previousDocumentId, RevisionId previousRevisionId,
    velocypack::Slice previousDocument, RevisionId newRevisionId,
    velocypack::Slice newDocument, OperationOptions const& options,
    TRI_voc_document_operation_e opType) {
  TRI_ASSERT(previousRevisionId.isSet());
  TRI_ASSERT(previousDocument.isObject());
  TRI_ASSERT(newRevisionId.isSet());
  TRI_ASSERT(newDocument.isObject());
  ::verifyDocumentStructure(newDocument,
                            _logicalCollection.type() == TRI_COL_TYPE_EDGE);

  LocalDocumentId newDocumentId =
      ::generateDocumentId(_logicalCollection, newRevisionId);

  auto* state = RocksDBTransactionState::toState(&trx);
  RocksDBTransactionStateGuard transactionStateGuard(state);

  TRI_ASSERT(!state->isReadOnlyTransaction());

  RocksDBSavePoint savepoint(_logicalCollection.id(), *state, opType);

  return modifyDocument(&trx, indexesSnapshot, savepoint, previousDocumentId,
                        previousDocument, newDocumentId, newDocument,
                        previousRevisionId, newRevisionId, options);
}

Result RocksDBCollection::remove(transaction::Methods& trx,
                                 IndexesSnapshot const& indexesSnapshot,
                                 LocalDocumentId previousDocumentId,
                                 RevisionId previousRevisionId,
                                 velocypack::Slice previousDocument,
                                 OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_remove_sec.count(time); });

  TRI_ASSERT(previousDocumentId.isSet());
  TRI_ASSERT(previousDocument.isObject());

  auto* state = RocksDBTransactionState::toState(&trx);
  RocksDBSavePoint savepoint(_logicalCollection.id(), *state,
                             TRI_VOC_DOCUMENT_OPERATION_REMOVE);

  return removeDocument(&trx, indexesSnapshot, savepoint, previousDocumentId,
                        previousDocument, options, previousRevisionId);
}

bool RocksDBCollection::cacheEnabled() const noexcept {
  return _cacheEnabled.load(std::memory_order_relaxed);
}

bool RocksDBCollection::hasDocuments() {
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(objectId());
  return rocksutils::hasKeys(
      _logicalCollection.vocbase().engine<RocksDBEngine>().db(), bounds,
      /*snapshot*/ nullptr, true);
}

/// @brief return engine-specific figures
void RocksDBCollection::figuresSpecific(
    bool details, arangodb::velocypack::Builder& builder) {
  rocksdb::TransactionDB* db =
      _logicalCollection.vocbase().engine<RocksDBEngine>().db();
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(objectId());
  rocksdb::Range r(bounds.start(), bounds.end());

  uint64_t out = 0;

  rocksdb::SizeApproximationOptions options{.include_memtables = true,
                                            .include_files = true};
  db->GetApproximateSizes(options,
                          RocksDBColumnFamilyManager::get(
                              RocksDBColumnFamilyManager::Family::Documents),
                          &r, 1, &out);

  builder.add("documentsSize", VPackValue(out));
  auto cache = useCache();
  builder.add("cacheInUse", VPackValue(cache != nullptr));
  if (cache != nullptr) {
    builder.add("cacheSize", VPackValue(cache->size()));
    builder.add("cacheUsage", VPackValue(cache->usage()));
    auto hitRates = cache->hitRates();
    double rate = hitRates.first;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheLifeTimeHitRate", VPackValue(rate));
    rate = hitRates.second;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheWindowedHitRate", VPackValue(rate));
  } else {
    builder.add("cacheSize", VPackValue(0));
    builder.add("cacheUsage", VPackValue(0));
  }

  if (details) {
    // engine-specific stuff here
    RocksDBFilePurgePreventer purgePreventer(
        _logicalCollection.vocbase().engine<RocksDBEngine>().disallowPurging());

    rocksdb::DB* rootDB = db->GetRootDB();

    // acquire a snapshot
    rocksdb::Snapshot const* snapshot = db->GetSnapshot();

    try {
      builder.add("engine", VPackValue(VPackValueType::Object));

      builder.add("documents",
                  VPackValue(rocksutils::countKeyRange(
                      rootDB, RocksDBKeyBounds::CollectionDocuments(objectId()),
                      snapshot, true)));
      builder.add("indexes", VPackValue(VPackValueType::Array));

      auto indexesSnapshot = getIndexesSnapshot();
      auto const& indexes = indexesSnapshot.getIndexes();

      for (auto const& it : indexes) {
        auto type = it->type();
        if (type == Index::TRI_IDX_TYPE_UNKNOWN ||
            type == Index::TRI_IDX_TYPE_IRESEARCH_LINK ||
            type == Index::TRI_IDX_TYPE_NO_ACCESS_INDEX) {
          continue;
        }

        builder.openObject();
        builder.add("type", VPackValue(it->typeName()));
        builder.add("id", VPackValue(it->id().id()));

        RocksDBIndex const* rix = static_cast<RocksDBIndex const*>(it.get());
        size_t count = 0;
        switch (type) {
          case Index::TRI_IDX_TYPE_INVERTED_INDEX: {
            auto snapshot =
                basics::downCast<iresearch::IResearchRocksDBInvertedIndex>(*rix)
                    .snapshot();
            count = snapshot.getDirectoryReader().live_docs_count();
          } break;
          case Index::TRI_IDX_TYPE_PRIMARY_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::PrimaryIndex(rix->objectId()), snapshot,
                true);
            break;
          case Index::TRI_IDX_TYPE_GEO_INDEX:
          case Index::TRI_IDX_TYPE_GEO1_INDEX:
          case Index::TRI_IDX_TYPE_GEO2_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::GeoIndex(rix->objectId()), snapshot,
                true);
            break;
          case Index::TRI_IDX_TYPE_ZKD_INDEX:
          case Index::TRI_IDX_TYPE_MDI_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::MdiIndex(rix->objectId()), snapshot,
                true);
            break;
          case Index::TRI_IDX_TYPE_MDI_PREFIXED_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::MdiVPackIndex(rix->objectId()), snapshot,
                true);
            break;
          case Index::TRI_IDX_TYPE_HASH_INDEX:
          case Index::TRI_IDX_TYPE_SKIPLIST_INDEX:
          case Index::TRI_IDX_TYPE_TTL_INDEX:
          case Index::TRI_IDX_TYPE_PERSISTENT_INDEX:
            if (it->unique()) {
              count = rocksutils::countKeyRange(
                  db,
                  RocksDBKeyBounds::UniqueVPackIndex(rix->objectId(), false),
                  snapshot, true);
            } else {
              count = rocksutils::countKeyRange(
                  db, RocksDBKeyBounds::VPackIndex(rix->objectId(), false),
                  snapshot, true);
            }
            break;
          case Index::TRI_IDX_TYPE_EDGE_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::EdgeIndex(rix->objectId()), snapshot,
                false);
            break;
          case Index::TRI_IDX_TYPE_FULLTEXT_INDEX:
            count = rocksutils::countKeyRange(
                db, RocksDBKeyBounds::FulltextIndex(rix->objectId()), snapshot,
                true);
            break;
          default:
            // we should not get here
            TRI_ASSERT(false);
        }

        builder.add("count", VPackValue(count));
        builder.close();
      }

      db->ReleaseSnapshot(snapshot);
    } catch (...) {
      // always free the snapshot
      db->ReleaseSnapshot(snapshot);
      throw;
    }
    builder.close();  // "indexes" array
    builder.close();  // "engine" object
  }
}

Result RocksDBCollection::insertDocument(transaction::Methods* trx,
                                         IndexesSnapshot const& indexesSnapshot,
                                         RocksDBSavePoint& savepoint,
                                         LocalDocumentId documentId,
                                         velocypack::Slice doc,
                                         OperationOptions const& options,
                                         RevisionId revisionId) const {
  savepoint.prepareOperation(revisionId);

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  Result res;

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthds = state->rocksdbMethods(_logicalCollection.id());

  auto const& indexes = indexesSnapshot.getIndexes();

  TRI_ASSERT(!options.checkUniqueConstraintsInPreflight ||
             state->isOnlyExclusiveTransaction());

  bool performPreflightChecks =
      (options.checkUniqueConstraintsInPreflight ||
       state->numOperations() >= ::preflightThreshold);

  if (performPreflightChecks) {
    // do a round of checks for all indexes, to verify that the insertion
    // will work (i.e. that there will be no unique constraint violations
    // later - we can't guard against disk full etc. later).
    // if this check already fails, there is no need to carry out the
    // actual index insertion, which will fail anyway, and in addition
    // spoil the current WriteBatch, which on a RollbackToSavePoint will
    // need to be completely reconstructed. the reconstruction of write
    // batches is super expensive, so we try to avoid it here.

    for (auto const& idx : indexes) {
      RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(idx.get());
      res = rIdx->checkInsert(*trx, mthds, documentId, doc, options);
      if (res.fail()) {
        return res;
      }
    }
  }

  TRI_ASSERT(res.ok());

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), documentId);
  TRI_ASSERT(key->containsLocalDocumentId(documentId));

  if (state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // banish new document to avoid caching without committing first
    invalidateCacheEntry(key.ref());
  }

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, state->isSingleOperation());

  TRI_IF_FAILURE("RocksDBCollection::insertFail1") {
    if (_logicalCollection.replicationVersion() == replication::Version::ONE ||
        _logicalCollection.isLeadingShard()) {
      // in replication2 the modification operations on the follower MUST NOT
      // fail if they do, we conclude that something else must have gone
      // terribly wrong and therefore abort the process
      if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
        return res.reset(TRI_ERROR_DEBUG);
      }
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::insertFail1Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (options.isRestore && ServerState::instance()->isDBServer()) {
    rocksdb::PinnableSlice ps;
    rocksdb::Status s =
        mthds->GetForUpdate(RocksDBColumnFamilyManager::get(
                                RocksDBColumnFamilyManager::Family::Documents),
                            key->string(), &ps);
    if (s.ok()) {
      // the LocalDocumentId should not have existed before...
      res.reset(TRI_ERROR_ARANGO_CONFLICT,
                "conflict with existing LocalDocumentId while trying to insert "
                "document on follower");
      res.withError([&doc](result::Error& err) {
        TRI_ASSERT(doc.get(StaticStrings::KeyString).isString());
        err.appendErrorMessage("; key: ");
        err.appendErrorMessage(doc.get(StaticStrings::KeyString).stringView());
      });
      TRI_ASSERT(false) << "insert: " << res.errorMessage()
                        << ", options: " << options;
    }
  }
#endif
  size_t const byteSize = static_cast<size_t>(doc.byteSize());

  rocksdb::Status s = mthds->PutUntracked(
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Documents),
      key.ref(), rocksdb::Slice(doc.startAs<char>(), byteSize));

  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::document));
    res.withError([&doc](result::Error& err) {
      TRI_ASSERT(doc.get(StaticStrings::KeyString).isString());
      err.appendErrorMessage("; key: ");
      err.appendErrorMessage(doc.get(StaticStrings::KeyString).stringView());
    });
    return res;
  }

  // we have successfully added a value to the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  trx->state()->trackShardUsage(
      *trx->resolver(), _logicalCollection.vocbase().name(),
      _logicalCollection.name(), trx->username(), AccessMode::Type::WRITE,
      "single-document insert", byteSize);

  {
    bool needReversal = false;
    auto reverse = [&](auto it) {
      if (needReversal && !state->isSingleOperation()) {
        ::reverseIdxOps(
            indexes, it,
            [mthds, trx, &documentId, &doc, &options](RocksDBIndex& rIdx) {
              return rIdx.remove(*trx, mthds, documentId, doc, options);
            });
      }
    };
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
      TRI_ASSERT(*it);
      TRI_IF_FAILURE("RocksDBCollection::insertFail2Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }
      TRI_IF_FAILURE("RocksDBCollection::insertFail2") {
        if (_logicalCollection.replicationVersion() ==
                replication::Version::ONE ||
            _logicalCollection.isLeadingShard()) {
          // in replication2 the modification operations on the follower MUST
          // NOT fail if they do, we conclude that something else must have gone
          // terribly wrong and therefore abort the process
          if (it == indexes.begin() &&
              RandomGenerator::interval(uint32_t(1000)) >= 995) {
            res.reset(TRI_ERROR_DEBUG);
            // reverse(it); TODO(MBkkt) remove first part of condition
            break;
          }
        }
      }
      auto& rIdx = basics::downCast<RocksDBIndex>(*it->get());
      // if we already performed the preflight checks,
      // there is no need to repeat the checks once again here
      res = rIdx.insert(*trx, mthds, documentId, doc, options,
                        /*performChecks*/ !performPreflightChecks);
      if (!res.ok()) {
        reverse(it);
        break;
      }
      needReversal = needReversal || rIdx.needsReversal();
    }
  }

  if (res.ok()) {
    TRI_ASSERT(revisionId == RevisionId::fromSlice(doc));

    res = savepoint.finish(revisionId);
    if (res.ok()) {
      state->trackInsert(_logicalCollection.id(), revisionId);
    }
  }

  return res;
}

Result RocksDBCollection::removeDocument(transaction::Methods* trx,
                                         IndexesSnapshot const& indexesSnapshot,
                                         RocksDBSavePoint& savepoint,
                                         LocalDocumentId documentId,
                                         velocypack::Slice doc,
                                         OperationOptions const& options,
                                         RevisionId revisionId) const {
  savepoint.prepareOperation(revisionId);

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);
  Result res;

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), documentId);
  TRI_ASSERT(key->containsLocalDocumentId(documentId));

  invalidateCacheEntry(key.ref());

  RocksDBMethods* mthds =
      RocksDBTransactionState::toMethods(trx, _logicalCollection.id());

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, trx->isSingleOperationTransaction());

  TRI_IF_FAILURE("RocksDBCollection::removeFail1") {
    if (_logicalCollection.replicationVersion() == replication::Version::ONE ||
        _logicalCollection.isLeadingShard()) {
      // in replication2 the modification operations on the follower MUST NOT
      // fail if they do, we conclude that something else must have gone
      // terribly wrong and therefore abort the process
      if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
        return res.reset(TRI_ERROR_DEBUG);
      }
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::removeFail1Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

  rocksdb::Status s =
      mthds->SingleDelete(RocksDBColumnFamilyManager::get(
                              RocksDBColumnFamilyManager::Family::Documents),
                          key.ref());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::document));
    res.withError([&doc](result::Error& err) {
      TRI_ASSERT(doc.get(StaticStrings::KeyString).isString());
      err.appendErrorMessage("; key: ");
      err.appendErrorMessage(doc.get(StaticStrings::KeyString).stringView());
    });
    return res;
  }

  // we have successfully removed a value from the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  trx->state()->trackShardUsage(
      *trx->resolver(), _logicalCollection.vocbase().name(),
      _logicalCollection.name(), trx->username(), AccessMode::Type::WRITE,
      "document remove", key->size());

  auto const& indexes = indexesSnapshot.getIndexes();

  {
    bool needReversal = false;
    auto reverse = [&](auto it) {
      if (needReversal && !trx->isSingleOperationTransaction()) {
        ::reverseIdxOps(
            indexes, it, [mthds, trx, &documentId, &doc](RocksDBIndex& rIdx) {
              OperationOptions options;
              options.indexOperationMode = IndexOperationMode::rollback;
              return rIdx.insert(*trx, mthds, documentId, doc, options,
                                 /*performChecks*/ true);
            });
      }
    };
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
      TRI_ASSERT(*it);
      TRI_IF_FAILURE("RocksDBCollection::removeFail2Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }
      TRI_IF_FAILURE("RocksDBCollection::removeFail2") {
        if (_logicalCollection.replicationVersion() ==
                replication::Version::ONE ||
            _logicalCollection.isLeadingShard()) {
          // in replication2 the modification operations on the follower MUST
          // NOT fail if they do, we conclude that something else must have gone
          // terribly wrong and therefore abort the process
          if (it == indexes.begin() &&
              RandomGenerator::interval(uint32_t(1000)) >= 995) {
            res.reset(TRI_ERROR_DEBUG);
            // reverse(it); TODO(MBkkt) remove first part of condition
            break;
          }
        }
      }
      auto& rIdx = basics::downCast<RocksDBIndex>(*it->get());
      res = rIdx.remove(*trx, mthds, documentId, doc, options);
      if (!res.ok()) {
        reverse(it);
        break;
      }
      needReversal = needReversal || rIdx.needsReversal();
    }
  }

  if (res.ok()) {
    TRI_ASSERT(revisionId == RevisionId::fromSlice(doc));

    res = savepoint.finish(_logicalCollection.newRevisionId());
    if (res.ok()) {
      RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
      state->trackRemove(_logicalCollection.id(), revisionId);
    }
  }

  return res;
}

Result RocksDBCollection::modifyDocument(
    transaction::Methods* trx, IndexesSnapshot const& indexesSnapshot,
    RocksDBSavePoint& savepoint, LocalDocumentId oldDocumentId,
    velocypack::Slice oldDoc, LocalDocumentId newDocumentId,
    velocypack::Slice newDoc, RevisionId oldRevisionId,
    RevisionId newRevisionId, OperationOptions const& options) const {
  Result res;

  if (oldDoc.get(StaticStrings::KeyString).stringView() !=
      newDoc.get(StaticStrings::KeyString).stringView()) {
    res.reset(TRI_ERROR_INTERNAL,
              absl::StrCat("invalid document update in '",
                           _logicalCollection.vocbase().name(), "/",
                           _logicalCollection.name()));
    res.withError([&oldDoc, &newDoc](result::Error& err) {
      err.appendErrorMessage("; old key: ");
      err.appendErrorMessage(oldDoc.get(StaticStrings::KeyString).stringView());
      err.appendErrorMessage("; new key: ");
      err.appendErrorMessage(newDoc.get(StaticStrings::KeyString).stringView());
    });
    TRI_ASSERT(false) << res.errorMessage();
    return res;
  }

  savepoint.prepareOperation(newRevisionId);

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthds = state->rocksdbMethods(_logicalCollection.id());

  auto const& indexes = indexesSnapshot.getIndexes();

  TRI_ASSERT(!options.checkUniqueConstraintsInPreflight ||
             state->isOnlyExclusiveTransaction());

  bool const performPreflightChecks =
      (options.checkUniqueConstraintsInPreflight ||
       state->numOperations() >= ::preflightThreshold);

  if (performPreflightChecks) {
    // do a round of checks for all indexes, to verify that the insertion
    // will work (i.e. that there will be no unique constraint violations
    // later - we can't guard against disk full etc. later).
    // if this check already fails, there is no need to carry out the
    // actual index insertion, which will fail anyway, and in addition
    // spoil the current WriteBatch, which on a RollbackToSavePoint will
    // need to be completely reconstructed. the reconstruction of write
    // batches is super expensive, so we try to avoid it here.
    for (auto const& idx : indexes) {
      RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(idx.get());
      res = rIdx->checkReplace(*trx, mthds, oldDocumentId, newDoc, options);
      if (res.fail()) {
        return res;
      }
    }
  }

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, trx->isSingleOperationTransaction());

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), oldDocumentId);
  TRI_ASSERT(key->containsLocalDocumentId(oldDocumentId));
  invalidateCacheEntry(key.ref());

  TRI_IF_FAILURE("RocksDBCollection::modifyFail1") {
    if (_logicalCollection.replicationVersion() == replication::Version::ONE ||
        _logicalCollection.isLeadingShard()) {
      // in replication2 the modification operations on the follower MUST NOT
      // fail if they do, we conclude that something else must have gone
      // terribly wrong and therefore abort the process
      if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
        return res.reset(TRI_ERROR_DEBUG);
      }
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::modifyFail1Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

  rocksdb::Status s =
      mthds->SingleDelete(RocksDBColumnFamilyManager::get(
                              RocksDBColumnFamilyManager::Family::Documents),
                          key.ref());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::document));
    res.withError([&newDoc](result::Error& err) {
      TRI_ASSERT(newDoc.get(StaticStrings::KeyString).isString());
      err.appendErrorMessage("; key: ");
      err.appendErrorMessage(newDoc.get(StaticStrings::KeyString).stringView());
    });
    return res;
  }

  // we have successfully removed a value from the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  TRI_IF_FAILURE("RocksDBCollection::modifyFail3") {
    if (_logicalCollection.replicationVersion() == replication::Version::ONE ||
        _logicalCollection.isLeadingShard()) {
      // in replication2 the modification operations on the follower MUST NOT
      // fail if they do, we conclude that something else must have gone
      // terribly wrong and therefore abort the process
      if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
        return res.reset(TRI_ERROR_DEBUG);
      }
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::modifyFail3Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

  key->constructDocument(objectId(), newDocumentId);
  TRI_ASSERT(key->containsLocalDocumentId(newDocumentId));

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (options.isRestore && ServerState::instance()->isDBServer()) {
    rocksdb::PinnableSlice ps;
    rocksdb::Status s =
        mthds->GetForUpdate(RocksDBColumnFamilyManager::get(
                                RocksDBColumnFamilyManager::Family::Documents),
                            key->string(), &ps);
    if (s.ok()) {
      // the LocalDocumentId should not have existed before...
      res.reset(TRI_ERROR_ARANGO_CONFLICT,
                "conflict with existing LocalDocumentId while trying to modify "
                "document on follower");
      res.withError([&oldDoc, &newDoc](result::Error& err) {
        TRI_ASSERT(oldDoc.get(StaticStrings::KeyString).isString());
        TRI_ASSERT(newDoc.get(StaticStrings::KeyString).isString());
        err.appendErrorMessage("; old key: ");
        err.appendErrorMessage(
            oldDoc.get(StaticStrings::KeyString).stringView());
        err.appendErrorMessage("; new key: ");
        err.appendErrorMessage(
            newDoc.get(StaticStrings::KeyString).stringView());
      });
      TRI_ASSERT(false) << "modify: " << res.errorMessage()
                        << ", options: " << options;
    }
  }
#endif

  size_t const byteSize = static_cast<size_t>(newDoc.byteSize());

  s = mthds->PutUntracked(RocksDBColumnFamilyManager::get(
                              RocksDBColumnFamilyManager::Family::Documents),
                          key.ref(),
                          rocksdb::Slice(newDoc.startAs<char>(), byteSize));
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  if (state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // banish new document to avoid caching without committing first
    invalidateCacheEntry(key.ref());
  }

  trx->state()->trackShardUsage(
      *trx->resolver(), _logicalCollection.vocbase().name(),
      _logicalCollection.name(), trx->username(), AccessMode::Type::WRITE,
      "document update/replace", byteSize);

  {
    bool needReversal = false;
    auto reverse = [&](auto it) {
      if (needReversal && !trx->isSingleOperationTransaction()) {
        ::reverseIdxOps(indexes, it, [&](RocksDBIndex& rIdx) {
          return rIdx.update(*trx, mthds, newDocumentId, newDoc, oldDocumentId,
                             oldDoc, options,
                             /*performChecks*/ true);
        });
      }
    };
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
      TRI_ASSERT(*it);
      TRI_IF_FAILURE("RocksDBCollection::modifyFail2Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }
      TRI_IF_FAILURE("RocksDBCollection::modifyFail2") {
        if (_logicalCollection.replicationVersion() ==
                replication::Version::ONE ||
            _logicalCollection.isLeadingShard()) {
          // in replication2 the modification operations on the follower MUST
          // NOT fail if they do, we conclude that something else must have gone
          // terribly wrong and therefore abort the process
          if (it == indexes.begin() &&
              RandomGenerator::interval(uint32_t(1000)) >= 995) {
            res.reset(TRI_ERROR_DEBUG);
            // reverse(it); TODO(MBkkt) remove first part of condition
            break;
          }
        }
      }
      auto& rIdx = basics::downCast<RocksDBIndex>(*it->get());
      // if we already performed the preflight checks,
      // there is no need to repeat the checks once again here
      res = rIdx.update(*trx, mthds, oldDocumentId, oldDoc, newDocumentId,
                        newDoc, options,
                        /*performChecks*/ !performPreflightChecks);
      if (!res.ok()) {
        reverse(it);
        break;
      }
      needReversal = needReversal || rIdx.needsReversal();
    }
  }

  if (res.ok()) {
    TRI_ASSERT(newRevisionId == RevisionId::fromSlice(newDoc));

    res = savepoint.finish(newRevisionId);
    if (res.ok()) {
      state->trackRemove(_logicalCollection.id(), oldRevisionId);
      state->trackInsert(_logicalCollection.id(), newRevisionId);
    }
  }

  return res;
}

Result RocksDBCollection::lookupDocumentVPack(
    transaction::Methods* trx, LocalDocumentId token,
    IndexIterator::DocumentCallback const& cb, LookupOptions options,
    StorageSnapshot const* snapshot) const {
  TRI_ASSERT(options.readCache || !options.fillCache);
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), token);

  std::shared_ptr<cache::Cache> cache;
  if (options.readCache && (cache = useCache())) {
    // check cache first for fast path
    auto f = cache->find(key->string().data(),
                         static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      cb(token, nullptr,
         VPackSlice(reinterpret_cast<uint8_t const*>(f.value()->value())));

      trx->state()->trackShardUsage(
          *trx->resolver(), _logicalCollection.vocbase().name(),
          _logicalCollection.name(), trx->username(), AccessMode::Type::READ,
          "document lookup from cache", f.value()->size());
      return {};
    }
  }

  transaction::StringLeaser buffer(trx);
  rocksdb::PinnableSlice ps(buffer.get());

  RocksDBMethods* mthd =
      RocksDBTransactionState::toMethods(trx, _logicalCollection.id());
  auto* family = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Documents);
  rocksdb::Status s =
      snapshot
          ? mthd->SingleGet(
                static_cast<RocksDBEngine::RocksDBSnapshot const*>(snapshot)
                    ->getSnapshot(),
                *family, key->string(), ps)
          : mthd->Get(family, key->string(), &ps,
                      static_cast<ReadOwnWrites>(options.readOwnWrites));

  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  if (options.countBytes) {
    trx->state()->trackShardUsage(
        *trx->resolver(), _logicalCollection.vocbase().name(),
        _logicalCollection.name(), trx->username(), AccessMode::Type::READ,
        "document lookup", ps.size());
  }

  TRI_ASSERT(ps.size() > 0);
  if (options.fillCache && cache != nullptr &&
      ps.size() <= _maxCacheValueSize) {
    // write entry back to cache
    cache::Cache::SimpleInserter<DocumentCacheType>{
        static_cast<DocumentCacheType&>(*cache), key->string().data(),
        static_cast<uint32_t>(key->string().size()), ps.data(),
        static_cast<uint64_t>(ps.size())};
  }
  cache.reset();

  VPackSlice doc{reinterpret_cast<uint8_t const*>(ps.data())};
  if (ps.IsPinned()) {
    cb(token, nullptr, doc);
    return {};
  }

  // TODO(MBkkt) in case of exception data will be destroyed
  //  We can avoid it (and return to the buffer instead), but is it worth?
  auto data = buffer.release();
  // move twice is intentionally
  cb(token, std::move(data), doc);
  buffer.acquire(std::move(data));

  return {};
}

void RocksDBCollection::setupCache() const {
  TRI_ASSERT(_cacheManager != nullptr);
  if (!_cacheEnabled.load(std::memory_order_relaxed)) {
    // if we cannot have a cache, return immediately
    return;
  }

  // there will never be a cache on the coordinator. this should be handled
  // by _cacheEnabled already.
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  auto cache = _cache;
  if (cache == nullptr) {
    TRI_ASSERT(_cacheManager != nullptr);
    TRI_ASSERT(_cacheManager->options().cacheSize > 0);
    TRI_ASSERT(_cacheManager->options().maxCacheValueSize > 0);
    LOG_TOPIC("f5df2", DEBUG, Logger::CACHE) << "Creating document cache";
    cache = _cacheManager->createCache<cache::BinaryKeyHasher>(
        cache::CacheType::Transactional);
    std::atomic_store_explicit(&_cache, std::move(cache),
                               std::memory_order_relaxed);
  }
}

std::shared_ptr<cache::Cache> RocksDBCollection::useCache() const noexcept {
  // note: this can return a nullptr. the caller has to check the result
  return std::atomic_load_explicit(&_cache, std::memory_order_relaxed);
}

void RocksDBCollection::destroyCache() const {
  auto cache = _cache;
  if (cache != nullptr) {
    std::atomic_store_explicit(&_cache, {}, std::memory_order_relaxed);
    TRI_ASSERT(_cacheManager != nullptr);
    LOG_TOPIC("7137b", DEBUG, Logger::CACHE) << "Destroying document cache";
    _cacheManager->destroyCache(std::move(cache));
  }
}

// banish given key from transactional cache
void RocksDBCollection::invalidateCacheEntry(RocksDBKey const& k) const {
  if (std::shared_ptr<cache::Cache> cache = useCache()) {
    do {
      auto status = cache->banish(k.buffer()->data(),
                                  static_cast<uint32_t>(k.buffer()->size()));
      if (status == TRI_ERROR_NO_ERROR ||
          status == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) {
        break;
      } else if (status == TRI_ERROR_SHUTTING_DOWN) {
        destroyCache();
        break;
      }
    } while (true);
  }
}

}  // namespace arangodb
