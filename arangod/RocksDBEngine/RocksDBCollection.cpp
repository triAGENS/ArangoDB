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
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "Random/RandomGenerator.h"
#include "RestServer/DatabaseFeature.h"
#include "Metrics/Counter.h"
#include "Metrics/Histogram.h"
#include "Metrics/LogScale.h"
#include "Metrics/MetricsFeature.h"
#include "RocksDBEngine/RocksDBBuilderIndex.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBIterators.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
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
#include "Utils/CollectionGuard.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/Identifiers/RevisionId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/ticks.h"
#include "VocBase/voc-types.h"

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
void reverseIdxOps(PhysicalCollection::IndexContainerType const& indexes,
                   PhysicalCollection::IndexContainerType::const_iterator& it,
                   F&& op) {
  while (it != indexes.begin()) {
    it--;
    auto* rIdx = static_cast<RocksDBIndex*>(it->get());
    if (rIdx->needsReversal()) {
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

void reportPrimaryIndexInconsistencyIfNotFound(Result const& res,
                                               std::string_view key,
                                               LocalDocumentId const& rev) {
  if (res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
    // Scandal! A primary index entry is pointing to nowhere! Let's report
    // this to the authorities immediately:
    LOG_TOPIC("42536", ERR, arangodb::Logger::ENGINES)
        << "Found primary index entry for which there is no actual "
           "document: _key="
        << key << ", _rev=" << rev.id();
    TRI_ASSERT(false);
  }
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
      _cacheEnabled(_cacheManager != nullptr && !collection.system() &&
                    !collection.isAStub() &&
                    !ServerState::instance()->isCoordinator() &&
                    basics::VelocyPackHelper::getBooleanValue(
                        info, StaticStrings::CacheEnabled, false)),
      _statistics(collection.vocbase()
                      .server()
                      .getFeature<metrics::MetricsFeature>()
                      .serverStatistics()
                      ._transactionsStatistics) {
  TRI_ASSERT(_logicalCollection.isAStub() || objectId() != 0);
  if (_cacheEnabled) {
    setupCache();
  }
}

RocksDBCollection::RocksDBCollection(LogicalCollection& collection,
                                     PhysicalCollection const* physical)
    : RocksDBMetaCollection(collection, VPackSlice::emptyObjectSlice()),
      _primaryIndex(nullptr),
      _cacheManager(collection.vocbase()
                        .server()
                        .getFeature<CacheManagerFeature>()
                        .manager()),
      _cacheEnabled(
          _cacheManager != nullptr && !collection.system() &&
          !collection.isAStub() && !ServerState::instance()->isCoordinator() &&
          static_cast<RocksDBCollection const*>(physical)->_cacheEnabled),
      _statistics(collection.vocbase()
                      .server()
                      .getFeature<metrics::MetricsFeature>()
                      .serverStatistics()
                      ._transactionsStatistics) {
  TRI_ASSERT(ServerState::instance()->isRunningInCluster());
  if (_cacheEnabled) {
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

Result RocksDBCollection::updateProperties(velocypack::Slice slice) {
  _cacheEnabled = _cacheManager != nullptr && !_logicalCollection.system() &&
                  !_logicalCollection.isAStub() &&
                  !ServerState::instance()->isCoordinator() &&
                  basics::VelocyPackHelper::getBooleanValue(
                      slice, StaticStrings::CacheEnabled, _cacheEnabled);
  primaryIndex()->setCacheEnabled(_cacheEnabled);

  if (_cacheEnabled) {
    setupCache();
    primaryIndex()->setupCache();
  } else {
    // will do nothing if cache is not present
    destroyCache();
    primaryIndex()->destroyCache();
    TRI_ASSERT(_cache == nullptr);
  }

  // nothing else to do
  return {};
}

PhysicalCollection* RocksDBCollection::clone(LogicalCollection& logical) const {
  return new RocksDBCollection(logical, this);
}

/// @brief export properties
void RocksDBCollection::getPropertiesVPack(velocypack::Builder& result) const {
  TRI_ASSERT(result.isOpenObject());
  result.add(StaticStrings::ObjectId, VPackValue(std::to_string(objectId())));
  result.add(StaticStrings::CacheEnabled, VPackValue(_cacheEnabled));
  TRI_ASSERT(result.isOpenObject());
}

/// @brief closes an open collection
ErrorCode RocksDBCollection::close() {
  RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
  for (auto it : _indexes) {
    it->unload();
  }
  return TRI_ERROR_NO_ERROR;
}

/// return bounds for all documents
RocksDBKeyBounds RocksDBCollection::bounds() const {
  return RocksDBKeyBounds::CollectionDocuments(objectId());
}

void RocksDBCollection::prepareIndexes(
    arangodb::velocypack::Slice indexesSlice) {
  TRI_ASSERT(indexesSlice.isArray());

  auto& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  std::vector<std::shared_ptr<Index>> indexes;
  {
    // link creation needs read-lock too
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
    if (indexesSlice.length() == 0 && _indexes.empty()) {
      engine.indexFactory().fillSystemIndexes(_logicalCollection, indexes);
    } else {
      engine.indexFactory().prepareIndexes(_logicalCollection, indexesSlice,
                                           indexes);
    }
  }

  RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
  TRI_ASSERT(_indexes.empty());
  for (std::shared_ptr<Index>& idx : indexes) {
    TRI_ASSERT(idx != nullptr);
    auto const id = idx->id();
    for (auto const& it : _indexes) {
      TRI_ASSERT(it != nullptr);
      if (it->id() == id) {  // index is there twice
        idx.reset();
        break;
      }
    }

    if (idx) {
      TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id.id()));
      _indexes.emplace(idx);
      if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
        TRI_ASSERT(idx->id().isPrimary());
        _primaryIndex = static_cast<RocksDBPrimaryIndex*>(idx.get());
      }
    }
  }

  auto it = _indexes.cbegin();
  if ((*it)->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX ||
      (TRI_COL_TYPE_EDGE == _logicalCollection.type() &&
       (_indexes.size() < 3 ||
        ((*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX ||
         (*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX)))) {
    std::string msg = "got invalid indexes for collection '" +
                      _logicalCollection.name() + "'";
    LOG_TOPIC("0ef34", ERR, arangodb::Logger::ENGINES) << msg;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    for (auto const& it : _indexes) {
      LOG_TOPIC("19e0b", ERR, arangodb::Logger::ENGINES)
          << "- " << it->context();
    }
#endif
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
  }

  TRI_ASSERT(!_indexes.empty());
}

Result RocksDBCollection::updateIndex(IndexId iid, VPackSlice body,
                                      VPackBuilder& updated) {
  TRI_ASSERT(body.isObject());
  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  if (!vocbase.use()) {  // someone dropped the database
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  bool const inRecovery = engine.inRecovery();

  std::unique_ptr<CollectionGuard> guard;
  try {
    guard =
        std::make_unique<CollectionGuard>(&vocbase, _logicalCollection.id());
  } catch (...) {
    vocbase.release();
    throw;
  }
  auto colGuard = scopeGuard([&]() noexcept { vocbase.release(); });
  READ_LOCKER(inventoryLocker, vocbase._inventoryLock);
  Result res;
  std::shared_ptr<arangodb::Index> toUpdate;
  {
    RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
    for (auto& it : _indexes) {
      if (iid == it->id()) {
        toUpdate = it;
        
        break;
      }
    }

    if (!toUpdate) {  // index not found
      //events::UpdateIndex(_logicalCollection.vocbase().name(),
      //                  _logicalCollection.name(), std::to_string(iid.id()),
      //                  TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
      return {TRI_ERROR_ARANGO_INDEX_NOT_FOUND};
    }

    if (!toUpdate->canBeUpdated()) {
      // FIXME: Do we need an event here?
      // events::UpdateIndex(collection->vocbase().name(), collection->name(),
      //                  std::to_string(iid.id()), TRI_ERROR_NOT_IMPLEMENTED);
      return Result(TRI_ERROR_NOT_IMPLEMENTED);
    }

    res = toUpdate->updateProperties(body);
    if (res.fail()) {
      return res;
    }
    // skip writing WAL marker if inRecovery()
    if (!inRecovery) {
      auto builder = _logicalCollection.toVelocyPackIgnore(
          {"path", "statusString"},
          LogicalDataSource::Serialization::PersistenceWithInProgress);
      // log this event in the WAL and in the collection meta-data
      toUpdate->toVelocyPack(updated,
                           Index::makeFlags(Index::Serialize::Internals));
      res = engine.writeCreateCollectionMarker(  // write marker
          _logicalCollection.vocbase().id(),            // vocbase id
          _logicalCollection.id(),                      // collection id
          builder.slice(),                              // RocksDB path
          RocksDBLogValue::IndexChange(                   // marker
              _logicalCollection.vocbase().id(), _logicalCollection.id(), iid,
              updated.slice()  // args
              ));
    }
  }
  return res;
}

std::shared_ptr<Index> RocksDBCollection::createIndex(VPackSlice info,
                                                      bool restore,
                                                      bool& created) {
  TRI_ASSERT(info.isObject());

  // Step 0. Lock all the things
  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  if (!vocbase.use()) {  // someone dropped the database
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  std::unique_ptr<CollectionGuard> guard;
  try {
    guard =
        std::make_unique<CollectionGuard>(&vocbase, _logicalCollection.id());
  } catch (...) {
    vocbase.release();
    throw;
  }

  auto colGuard = scopeGuard([&]() noexcept { vocbase.release(); });

  READ_LOCKER(inventoryLocker, vocbase._inventoryLock);

  RocksDBBuilderIndex::Locker locker(this);
  if (!locker.lock()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_LOCK_TIMEOUT);
  }

  auto& selector = vocbase.server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();

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
      return existingIdx;
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
  Result res = arangodb::basics::catchToResult([&]() -> Result {
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
        return res.reset(rocksutils::convertStatus(s));
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
        return res;
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
      res = buildIdx->fillIndexBackground(locker);
    } else {
      res = buildIdx->fillIndexForeground();
    }
    if (res.fail()) {
      return res;
    }

    // always (re-)lock to avoid inconsistencies
    locker.lock();

    inventoryLocker.lock();

    // Step 5. register in index list
    {
      RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
      if (inBackground) {
        // remove temporary index and swap in actual index
        removeIndex(_indexes, buildIdx->id());
      }
      _indexes.emplace(newIdx);
    }

    syncIndexOnCreate(*newIdx);

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

    return res;
  });

  if (res.ok()) {
    created = true;
    indexCleanup.cancel();
    return newIdx;
  }

  // cleanup routine
  // We could not create the index. Better abort
  {
    RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
    removeIndex(_indexes, newIdx->id());
  }
  THROW_ARANGO_EXCEPTION(res);
}

/// @brief Drop an index with the given iid.
bool RocksDBCollection::dropIndex(IndexId iid) {
  // usually always called when _exclusiveLock is held
  if (iid.empty() || iid.isPrimary()) {
    return true;
  }

  auto& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  bool const inRecovery = engine.inRecovery();

  std::shared_ptr<arangodb::Index> toRemove;
  {
    RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
    for (auto& it : _indexes) {
      if (iid == it->id()) {
        toRemove = it;
        _indexes.erase(it);
        break;
      }
    }

    if (!toRemove) {  // index not found
      // We tried to remove an index that does not exist
      events::DropIndex(_logicalCollection.vocbase().name(),
                        _logicalCollection.name(), std::to_string(iid.id()),
                        TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
      return false;
    }

    // skip writing WAL marker if inRecovery()
    if (!inRecovery) {
      auto builder = _logicalCollection.toVelocyPackIgnore(
          {"path", "statusString"},
          LogicalDataSource::Serialization::PersistenceWithInProgress);
      // log this event in the WAL and in the collection meta-data
      Result res = engine.writeCreateCollectionMarker(  // write marker
          _logicalCollection.vocbase().id(),            // vocbase id
          _logicalCollection.id(),                      // collection id
          builder.slice(),                              // RocksDB path
          RocksDBLogValue::IndexDrop(                   // marker
              _logicalCollection.vocbase().id(), _logicalCollection.id(),
              iid  // args
              ));

      if (res.fail()) {
        return false;
      }
    }
  }

  TRI_ASSERT(toRemove != nullptr);

  RocksDBIndex* cindex = static_cast<RocksDBIndex*>(toRemove.get());
  TRI_ASSERT(cindex != nullptr);

  Result res = cindex->drop();

  if (res.ok()) {
    events::DropIndex(_logicalCollection.vocbase().name(),
                      _logicalCollection.name(), std::to_string(iid.id()),
                      TRI_ERROR_NO_ERROR);

    if (meta().numberDocuments() >= 32 * 1024) {
      cindex->compact();  // trigger compaction to reclaim disk space
    }
  }

  return res.ok();
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

  EngineSelectorFeature& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  RocksDBEngine& engine = selector.engine<RocksDBEngine>();
  RocksDBReplicationManager* manager = engine.replicationManager();
  RocksDBReplicationContext* ctx =
      batchId == 0 ? nullptr : manager->find(batchId);
  auto guard = scopeGuard([manager, ctx]() noexcept {
    if (ctx) {
      manager->release(ctx);
    }
  });
  rocksdb::Snapshot const* snapshot = ctx ? ctx->snapshot() : nullptr;

  return std::make_unique<RocksDBRevisionReplicationIterator>(
      _logicalCollection, snapshot);
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
      this->canUseRangeDeleteInWal() && _meta.numberDocuments() >= 32 * 1024) {
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

  RocksDBEngine& engine = _logicalCollection.vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine<RocksDBEngine>();
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

  // delete index values
  {
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
    for (std::shared_ptr<Index> const& idx : _indexes) {
      RocksDBIndex* ridx = static_cast<RocksDBIndex*>(idx.get());
      bounds = ridx->getBounds();
      s = batch.DeleteRange(bounds.columnFamily(), bounds.start(),
                            bounds.end());
      if (!s.ok()) {
        return rocksutils::convertStatus(s);
      }
    }
  }

  // add the log entry so we can recover the correct count
  auto log = RocksDBLogValue::CollectionTruncate(
      trx.vocbase().id(), _logicalCollection.id(), objectId());

  s = batch.PutLogData(log.slice());
  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  rocksdb::WriteOptions wo;

  s = db->Write(wo, &batch);

  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  rocksdb::SequenceNumber seq =
      db->GetLatestSequenceNumber() - 1;  // post commit sequence

  uint64_t numDocs = _meta.numberDocuments();
  _meta.adjustNumberDocuments(seq,
                              /*revision*/ _logicalCollection.newRevisionId(),
                              -static_cast<int64_t>(numDocs));

  {
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
    for (std::shared_ptr<Index> const& idx : _indexes) {
      idx->afterTruncate(seq,
                         &trx);  // clears caches / clears links (if applicable)
    }
  }
  bufferTruncate(seq);

  TRI_ASSERT(!state->hasOperations());  // not allowed
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
  rocksdb::Slice const end = documentBounds.end();

  // avoid OOM error for truncate by committing earlier
  auto state = RocksDBTransactionState::toState(&trx);
  uint64_t const prvICC = state->options().intermediateCommitCount;
  if (!state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    state->options().intermediateCommitCount =
        std::min<uint64_t>(prvICC, 10000);
  }

  // push our current transaction on the stack
  state->beginQuery(true);
  auto stateGuard = scopeGuard([state, prvICC]() noexcept {
    state->endQuery(true);
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

  auto iter =
      mthds->NewIterator(documentBounds.columnFamily(), [&](ReadOptions& ro) {
        ro.iterate_upper_bound = &end;
        // we are going to blow away all data anyway. no need to blow up the
        // cache
        ro.fill_cache = false;
        ro.readOwnWrites = false;
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
      Result res = removeBufferedDocuments(keyBuffer, found);
      if (res.fail()) {
        return res;
      }
    }
  }

  if (found > 0) {
    Result res = removeBufferedDocuments(keyBuffer, found);
    if (res.fail()) {
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
  result.first = LocalDocumentId::none();
  result.second = RevisionId::none();

  // lookup the revision id in the primary index
  if (!primaryIndex()->lookupRevision(trx, key, result.first, result.second,
                                      readOwnWrites)) {
    // document not found
    TRI_ASSERT(!result.first.isSet());
    TRI_ASSERT(result.second.empty());
    return Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
  }

  // document found, but revisionId may not have been present in the primary
  // index. this can happen for "older" collections
  TRI_ASSERT(result.first.isSet());
  TRI_ASSERT(result.second.isSet());
  return {};
}

bool RocksDBCollection::lookupRevision(transaction::Methods* trx,
                                       VPackSlice const& key,
                                       RevisionId& revisionId,
                                       ReadOwnWrites readOwnWrites) const {
  TRI_ASSERT(key.isString());
  LocalDocumentId documentId;
  revisionId = RevisionId::none();
  // lookup the revision id in the primary index
  if (!primaryIndex()->lookupRevision(trx, key.stringView(), documentId,
                                      revisionId, readOwnWrites)) {
    // document not found
    TRI_ASSERT(revisionId.empty());
    return false;
  }

  // document found, and we have a valid revisionId
  TRI_ASSERT(documentId.isSet());
  TRI_ASSERT(revisionId.isSet());
  return true;
}

Result RocksDBCollection::read(transaction::Methods* trx, std::string_view key,
                               IndexIterator::DocumentCallback const& cb,
                               ReadOwnWrites readOwnWrites) const {
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
    documentId =
        primaryIndex()->lookupKey(trx, key, readOwnWrites, foundInCache);
    if (!documentId.isSet()) {
      res.reset(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
      return res;
    }  // else found

    TRI_IF_FAILURE("RocksDBCollection::read-delay") {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(RandomGenerator::interval(uint32_t(2000))));
    }

    res = lookupDocumentVPack(trx, documentId, ps, /*readCache*/ true,
                              /*fillCache*/ true, readOwnWrites);
    if (res.ok()) {
      cb(documentId, VPackSlice(reinterpret_cast<uint8_t const*>(ps.data())));
    }
  } while (res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
           RocksDBTransactionState::toState(trx)->ensureSnapshot());
  ::reportPrimaryIndexInconsistencyIfNotFound(res, key, documentId);
  return res;
}

// read using a local document id
Result RocksDBCollection::read(transaction::Methods* trx,
                               LocalDocumentId const& documentId,
                               IndexIterator::DocumentCallback const& cb,
                               ReadOwnWrites readOwnWrites) const {
  ::ReadTimeTracker timeTracker(
      _statistics._readWriteMetrics,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_read_sec.count(time); });

  if (!documentId.isSet()) {
    return Result{TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD,
                  "invalid local document id"};
  }

  return lookupDocumentVPack(trx, documentId, cb, /*withCache*/ true,
                             readOwnWrites);
}

Result RocksDBCollection::insert(transaction::Methods& trx,
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

  Result res = insertDocument(&trx, savepoint, newDocumentId, newDocument,
                              options, newRevisionId);

  if (res.ok()) {
    res = savepoint.finish(newRevisionId);
  }

  return res;
}

Result RocksDBCollection::update(transaction::Methods& trx,
                                 LocalDocumentId previousDocumentId,
                                 RevisionId previousRevisionId,
                                 velocypack::Slice previousDocument,
                                 RevisionId newRevisionId,
                                 velocypack::Slice newDocument,
                                 OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_update_sec.count(time); });

  return performUpdateOrReplace(trx, previousDocumentId, previousRevisionId,
                                previousDocument, newRevisionId, newDocument,
                                options, TRI_VOC_DOCUMENT_OPERATION_UPDATE);
}

Result RocksDBCollection::replace(transaction::Methods& trx,
                                  LocalDocumentId previousDocumentId,
                                  RevisionId previousRevisionId,
                                  velocypack::Slice previousDocument,
                                  RevisionId newRevisionId,
                                  velocypack::Slice newDocument,
                                  OperationOptions const& options) {
  ::WriteTimeTracker timeTracker(
      _statistics._readWriteMetrics, options,
      [](TransactionStatistics::ReadWriteMetrics& metrics,
         float time) noexcept { metrics.rocksdb_replace_sec.count(time); });

  return performUpdateOrReplace(trx, previousDocumentId, previousRevisionId,
                                previousDocument, newRevisionId, newDocument,
                                options, TRI_VOC_DOCUMENT_OPERATION_REPLACE);
}

Result RocksDBCollection::performUpdateOrReplace(
    transaction::Methods& trx, LocalDocumentId previousDocumentId,
    RevisionId previousRevisionId, velocypack::Slice previousDocument,
    RevisionId newRevisionId, velocypack::Slice newDocument,
    OperationOptions const& options, TRI_voc_document_operation_e opType) {
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

  Result res = modifyDocument(&trx, savepoint, previousDocumentId,
                              previousDocument, newDocumentId, newDocument,
                              previousRevisionId, newRevisionId, options);

  if (res.ok()) {
    res = savepoint.finish(newRevisionId);
  }

  return res;
}

Result RocksDBCollection::remove(transaction::Methods& trx,
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

  Result res = removeDocument(&trx, savepoint, previousDocumentId,
                              previousDocument, options, previousRevisionId);

  if (res.ok()) {
    res = savepoint.finish(_logicalCollection.newRevisionId());
  }

  return res;
}

bool RocksDBCollection::hasDocuments() {
  RocksDBEngine& engine = _logicalCollection.vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine<RocksDBEngine>();
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(objectId());
  return rocksutils::hasKeys(engine.db(), bounds, /*snapshot*/ nullptr, true);
}

/// @brief return engine-specific figures
void RocksDBCollection::figuresSpecific(
    bool details, arangodb::velocypack::Builder& builder) {
  auto& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  rocksdb::TransactionDB* db = engine.db();
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
  bool cacheInUse = useCache();
  builder.add("cacheInUse", VPackValue(cacheInUse));
  if (cacheInUse) {
    builder.add("cacheSize", VPackValue(_cache->size()));
    builder.add("cacheUsage", VPackValue(_cache->usage()));
    auto hitRates = _cache->hitRates();
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
    RocksDBFilePurgePreventer purgePreventer(engine.disallowPurging());

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

      RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
      for (auto const& it : _indexes) {
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

Result RocksDBCollection::insertDocument(arangodb::transaction::Methods* trx,
                                         RocksDBSavePoint& savepoint,
                                         LocalDocumentId documentId,
                                         VPackSlice doc,
                                         OperationOptions const& options,
                                         RevisionId revisionId) const {
  savepoint.prepareOperation(revisionId);

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  Result res;

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthds = state->rocksdbMethods(_logicalCollection.id());

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
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    for (auto const& idx : _indexes) {
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
    if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
      return res.reset(TRI_ERROR_DEBUG);
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::insertFail1Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

  rocksdb::Status s = mthds->PutUntracked(
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Documents),
      key.ref(),
      rocksdb::Slice(doc.startAs<char>(), static_cast<size_t>(doc.byteSize())));
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::document));
    res.withError([&doc](result::Error& err) {
      TRI_ASSERT(doc.get(StaticStrings::KeyString).isString());
      err.appendErrorMessage("; key: ");
      err.appendErrorMessage(doc.get(StaticStrings::KeyString).copyString());
    });
    return res;
  }

  // we have successfully added a value to the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  {
    bool needReversal = false;
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    for (auto it = _indexes.begin(); it != _indexes.end(); ++it) {
      TRI_IF_FAILURE("RocksDBCollection::insertFail2") {
        if (it == _indexes.begin() &&
            RandomGenerator::interval(uint32_t(1000)) >= 995) {
          return res.reset(TRI_ERROR_DEBUG);
        }
      }

      TRI_IF_FAILURE("RocksDBCollection::insertFail2Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }

      RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(it->get());
      // if we already performed the preflight checks, there is no need to
      // repeat the checks once again here
      res = rIdx->insert(*trx, mthds, documentId, doc, options,
                         /*performChecks*/ !performPreflightChecks);
      // currently only IResearchLink indexes need a reversal
      needReversal = needReversal || rIdx->needsReversal();

      if (res.fail()) {
        if (needReversal && !state->isSingleOperation()) {
          ::reverseIdxOps(_indexes, it,
                          [mthds, trx, &documentId, &doc](RocksDBIndex* rIdx) {
                            return rIdx->remove(*trx, mthds, documentId, doc);
                          });
        }
        break;
      }
    }
  }

  if (res.ok()) {
    TRI_ASSERT(revisionId == RevisionId::fromSlice(doc));
    state->trackInsert(_logicalCollection.id(), revisionId);
  }

  return res;
}

Result RocksDBCollection::removeDocument(arangodb::transaction::Methods* trx,
                                         RocksDBSavePoint& savepoint,
                                         LocalDocumentId documentId,
                                         VPackSlice doc,
                                         OperationOptions const& /*options*/,
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
    if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
      return res.reset(TRI_ERROR_DEBUG);
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
      err.appendErrorMessage(doc.get(StaticStrings::KeyString).copyString());
    });
    return res;
  }

  // we have successfully removed a value from the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  {
    bool needReversal = false;
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
      TRI_IF_FAILURE("RocksDBCollection::removeFail2") {
        if (it == _indexes.begin() &&
            RandomGenerator::interval(uint32_t(1000)) >= 995) {
          return res.reset(TRI_ERROR_DEBUG);
        }
      }
      TRI_IF_FAILURE("RocksDBCollection::removeFail2Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }

      RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(it->get());
      res = rIdx->remove(*trx, mthds, documentId, doc);
      needReversal = needReversal || rIdx->needsReversal();
      if (res.fail()) {
        if (needReversal && !trx->isSingleOperationTransaction()) {
          ::reverseIdxOps(
              _indexes, it,
              [mthds, trx, &documentId, &doc](RocksDBIndex* rIdx) {
                OperationOptions options;
                options.indexOperationMode = IndexOperationMode::rollback;
                return rIdx->insert(*trx, mthds, documentId, doc, options,
                                    /*performChecks*/ true);
              });
        }
        break;
      }
    }
  }

  if (res.ok()) {
    RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
    TRI_ASSERT(revisionId == RevisionId::fromSlice(doc));
    state->trackRemove(_logicalCollection.id(), revisionId);
  }

  return res;
}

Result RocksDBCollection::modifyDocument(
    transaction::Methods* trx, RocksDBSavePoint& savepoint,
    LocalDocumentId oldDocumentId, VPackSlice oldDoc,
    LocalDocumentId newDocumentId, VPackSlice newDoc, RevisionId oldRevisionId,
    RevisionId newRevisionId, OperationOptions const& options) const {
  savepoint.prepareOperation(newRevisionId);

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);
  Result res;

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthds = state->rocksdbMethods(_logicalCollection.id());

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
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    for (auto const& idx : _indexes) {
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
    if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
      return res.reset(TRI_ERROR_DEBUG);
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
      err.appendErrorMessage(newDoc.get(StaticStrings::KeyString).copyString());
    });
    return res;
  }

  // we have successfully removed a value from the WBWI. after this, we
  // can only restore the previous state via a full rebuild
  savepoint.tainted();

  TRI_IF_FAILURE("RocksDBCollection::modifyFail2") {
    if (RandomGenerator::interval(uint32_t(1000)) >= 995) {
      return res.reset(TRI_ERROR_DEBUG);
    }
  }

  TRI_IF_FAILURE("RocksDBCollection::modifyFail2Always") {
    return res.reset(TRI_ERROR_DEBUG);
  }

  key->constructDocument(objectId(), newDocumentId);
  TRI_ASSERT(key->containsLocalDocumentId(newDocumentId));
  s = mthds->PutUntracked(
      RocksDBColumnFamilyManager::get(
          RocksDBColumnFamilyManager::Family::Documents),
      key.ref(),
      rocksdb::Slice(newDoc.startAs<char>(),
                     static_cast<size_t>(newDoc.byteSize())));
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  if (state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // banish new document to avoid caching without committing first
    invalidateCacheEntry(key.ref());
  }

  {
    bool needReversal = false;
    RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);

    for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
      TRI_IF_FAILURE("RocksDBCollection::modifyFail3") {
        if (it == _indexes.begin() &&
            RandomGenerator::interval(uint32_t(1000)) >= 995) {
          return res.reset(TRI_ERROR_DEBUG);
        }
      }

      TRI_IF_FAILURE("RocksDBCollection::modifyFail3Always") {
        return res.reset(TRI_ERROR_DEBUG);
      }

      auto rIdx = static_cast<RocksDBIndex*>(it->get());
      // if we already performed the preflight checks, there is no need to
      // repeat the checks once again here
      res = rIdx->update(*trx, mthds, oldDocumentId, oldDoc, newDocumentId,
                         newDoc, options,
                         /*performChecks*/ !performPreflightChecks);
      needReversal = needReversal || rIdx->needsReversal();
      if (!res.ok()) {
        if (needReversal && !trx->isSingleOperationTransaction()) {
          ::reverseIdxOps(_indexes, it, [&](RocksDBIndex* rIdx) {
            return rIdx->update(*trx, mthds, newDocumentId, newDoc,
                                oldDocumentId, oldDoc, options,
                                /*performChecks*/ true);
          });
        }
        break;
      }
    }
  }

  if (res.ok()) {
    TRI_ASSERT(newRevisionId == RevisionId::fromSlice(newDoc));
    state->trackRemove(_logicalCollection.id(), oldRevisionId);
    state->trackInsert(_logicalCollection.id(), newRevisionId);
  }

  return res;
}

/// @brief lookup document in cache and / or rocksdb
Result RocksDBCollection::lookupDocument(transaction::Methods& trx,
                                         LocalDocumentId documentId,
                                         velocypack::Builder& builder,
                                         bool readCache, bool fillCache,
                                         ReadOwnWrites readOwnWrites) const {
  TRI_ASSERT(trx.state()->isRunning());
  TRI_ASSERT(objectId() != 0);

  RocksDBKeyLeaser key(&trx);
  key->constructDocument(objectId(), documentId);

  bool lockTimeout = false;
  if (readCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {  // copy finding into buffer
      builder.add(
          VPackSlice(reinterpret_cast<uint8_t const*>(f.value()->value())));
      TRI_ASSERT(builder.slice().isObject());
      return {};  // all good
    }

    if (f.result() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthd =
      RocksDBTransactionState::toMethods(&trx, _logicalCollection.id());
  rocksdb::PinnableSlice ps;
  rocksdb::Status s =
      mthd->Get(RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Documents),
                key->string(), &ps, readOwnWrites);

  if (!s.ok()) {
    LOG_TOPIC("ba2ef", DEBUG, Logger::ENGINES)
        << "NOT FOUND rev: " << documentId.id()
        << " trx: " << trx.state()->id().id() << " objectID " << objectId()
        << " name: " << _logicalCollection.name();
    return rocksutils::convertStatus(s, rocksutils::document);
  }

  if (fillCache && useCache() && !lockTimeout) {
    TRI_ASSERT(_cache != nullptr);
    // write entry back to cache
    cache::Cache::SimpleInserter<DocumentCacheType>{
        static_cast<DocumentCacheType&>(*_cache), key->string().data(),
        static_cast<uint32_t>(key->string().size()), ps.data(),
        static_cast<uint64_t>(ps.size())};
  }

  builder.add(VPackSlice(reinterpret_cast<uint8_t const*>(ps.data())));
  TRI_ASSERT(builder.slice().isObject());

  return {};
}

/// @brief lookup document in cache and / or rocksdb
arangodb::Result RocksDBCollection::lookupDocumentVPack(
    transaction::Methods* trx, LocalDocumentId const& documentId,
    rocksdb::PinnableSlice& ps, bool readCache, bool fillCache,
    ReadOwnWrites readOwnWrites) const {
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);
  Result res;

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), documentId);

  bool lockTimeout = false;
  if (readCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {  // copy finding into buffer
      ps.PinSelf(
          rocksdb::Slice(reinterpret_cast<char const*>(f.value()->value()),
                         f.value()->valueSize()));
      // TODO we could potentially use the PinSlice method ?!
      return {};  // all good
    }
    if (f.result() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthd =
      RocksDBTransactionState::toMethods(trx, _logicalCollection.id());
  rocksdb::Status s =
      mthd->Get(RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Documents),
                key->string(), &ps, readOwnWrites);

  if (!s.ok()) {
    LOG_TOPIC("f63dd", DEBUG, Logger::ENGINES)
        << "NOT FOUND rev: " << documentId.id()
        << " trx: " << trx->state()->id().id() << " objectID " << objectId()
        << " name: " << _logicalCollection.name();
    return rocksutils::convertStatus(s, rocksutils::document);
  }

  if (fillCache && useCache() && !lockTimeout) {
    TRI_ASSERT(_cache != nullptr);
    // write entry back to cache
    cache::Cache::SimpleInserter<DocumentCacheType>{
        static_cast<DocumentCacheType&>(*_cache), key->string().data(),
        static_cast<uint32_t>(key->string().size()), ps.data(),
        static_cast<uint64_t>(ps.size())};
  }

  return {};
}

Result RocksDBCollection::lookupDocumentVPack(
    transaction::Methods* trx, LocalDocumentId const& documentId,
    IndexIterator::DocumentCallback const& cb, bool withCache,
    ReadOwnWrites readOwnWrites) const {
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(objectId() != 0);

  RocksDBKeyLeaser key(trx);
  key->constructDocument(objectId(), documentId);

  if (withCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      cb(documentId,
         VPackSlice(reinterpret_cast<uint8_t const*>(f.value()->value())));
      return {};
    }
  }

  transaction::StringLeaser buffer(trx);
  rocksdb::PinnableSlice ps(buffer.get());

  RocksDBMethods* mthd =
      RocksDBTransactionState::toMethods(trx, _logicalCollection.id());
  rocksdb::Status s =
      mthd->Get(RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Documents),
                key->string(), &ps, readOwnWrites);

  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  TRI_ASSERT(ps.size() > 0);
  cb(documentId, VPackSlice(reinterpret_cast<uint8_t const*>(ps.data())));

  if (withCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // write entry back to cache
    cache::Cache::SimpleInserter<DocumentCacheType>{
        static_cast<DocumentCacheType&>(*_cache), key->string().data(),
        static_cast<uint32_t>(key->string().size()), ps.data(),
        static_cast<uint64_t>(ps.size())};
  }

  return {};
}

void RocksDBCollection::setupCache() const {
  if (_cacheManager == nullptr || !_cacheEnabled) {
    // if we cannot have a cache, return immediately
    return;
  }

  // there will never be a cache on the coordinator. this should be handled
  // by _cacheEnabled already.
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  if (_cache == nullptr) {
    TRI_ASSERT(_cacheManager != nullptr);
    LOG_TOPIC("f5df2", DEBUG, Logger::CACHE) << "Creating document cache";
    _cache = _cacheManager->createCache<cache::BinaryKeyHasher>(
        cache::CacheType::Transactional);
  }
}

void RocksDBCollection::destroyCache() const {
  if (_cache != nullptr) {
    TRI_ASSERT(_cacheManager != nullptr);
    LOG_TOPIC("7137b", DEBUG, Logger::CACHE) << "Destroying document cache";
    _cacheManager->destroyCache(_cache);
    _cache.reset();
  }
}

// banish given key from transactional cache
void RocksDBCollection::invalidateCacheEntry(RocksDBKey const& k) const {
  if (useCache()) {
    TRI_ASSERT(_cache != nullptr);
    bool banished = false;
    while (!banished) {
      auto status = _cache->banish(k.buffer()->data(),
                                   static_cast<uint32_t>(k.buffer()->size()));
      if (status.ok()) {
        banished = true;
      } else if (status.errorNumber() == TRI_ERROR_SHUTTING_DOWN) {
        destroyCache();
        break;
      }
    }
  }
}

/// @brief can use non transactional range delete in write ahead log
bool RocksDBCollection::canUseRangeDeleteInWal() const {
  if (ServerState::instance()->isSingleServer()) {
    return true;
  }
  auto& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  return engine.useRangeDeleteInWal();
}

}  // namespace arangodb
