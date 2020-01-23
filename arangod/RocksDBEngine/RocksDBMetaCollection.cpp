////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////


#include "RocksDBMetaCollection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ReadLocker.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/hashes.h"
#include "Basics/system-functions.h"
#include "Cluster/ServerState.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBReplicationContext.h"
#include "RocksDBEngine/RocksDBReplicationManager.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"

#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

RocksDBMetaCollection::RocksDBMetaCollection(LogicalCollection& collection,
                                             VPackSlice const& info)
    : PhysicalCollection(collection, info),
      _objectId(basics::VelocyPackHelper::stringUInt64(info, "objectId")) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  VPackSlice s = info.get("isVolatile");
  if (s.isBoolean() && s.getBoolean()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "volatile collections are unsupported in the RocksDB engine");
  }
  
  TRI_ASSERT(_logicalCollection.isAStub() || _objectId != 0);
  rocksutils::globalRocksEngine()->addCollectionMapping(_objectId, _logicalCollection.vocbase().id(),
                                                        _logicalCollection.id());

  if (collection.syncByRevision()) {
    _revisionTree =
        std::make_unique<containers::RevisionTree>(6, collection.minRevision());
  }
}

RocksDBMetaCollection::RocksDBMetaCollection(LogicalCollection& collection,
                                             PhysicalCollection const* physical)
    : PhysicalCollection(collection, VPackSlice::emptyObjectSlice()),
      _objectId(static_cast<RocksDBMetaCollection const*>(physical)->_objectId) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  rocksutils::globalRocksEngine()->addCollectionMapping(_objectId, _logicalCollection.vocbase().id(), _logicalCollection.id());

  if (collection.syncByRevision()) {
    _revisionTree =
        std::make_unique<containers::RevisionTree>(6, collection.minRevision());
  }
}

std::string const& RocksDBMetaCollection::path() const {
  return StaticStrings::Empty;  // we do not have any path
}

TRI_voc_rid_t RocksDBMetaCollection::revision(transaction::Methods* trx) const {
  auto* state = RocksDBTransactionState::toState(trx);
  auto trxCollection = static_cast<RocksDBTransactionCollection*>(
                                                                  state->findCollection(_logicalCollection.id()));
  
  TRI_ASSERT(trxCollection != nullptr);
  
  return trxCollection->revision();
}

uint64_t RocksDBMetaCollection::numberDocuments(transaction::Methods* trx) const {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  auto* state = RocksDBTransactionState::toState(trx);
  auto trxCollection = static_cast<RocksDBTransactionCollection*>(
                                                                  state->findCollection(_logicalCollection.id()));
  
  TRI_ASSERT(trxCollection != nullptr);
  
  return trxCollection->numberDocuments();
}

/// @brief write locks a collection, with a timeout
int RocksDBMetaCollection::lockWrite(double timeout) {
  uint64_t waitTime = 0;  // indicates that time is uninitialized
  double startTime = 0.0;
  
  while (true) {
    TRY_WRITE_LOCKER(locker, _exclusiveLock);
    
    if (locker.isLocked()) {
      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }
    
    double now = TRI_microtime();
    
    if (waitTime == 0) {  // initialize times
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = defaultLockTimeout;
      }
      
      startTime = now;
      waitTime = 1;
    }
    
    if (now > startTime + timeout) {
      LOG_TOPIC("d1e53", TRACE, arangodb::Logger::ENGINES)
      << "timed out after " << timeout << " s waiting for write-lock on collection '"
      << _logicalCollection.name() << "'";
      
      return TRI_ERROR_LOCK_TIMEOUT;
    }
    
    if (now - startTime < 0.001) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
      if (waitTime < 32) {
        waitTime *= 2;
      }
    }
  }
}

/// @brief write unlocks a collection
void RocksDBMetaCollection::unlockWrite() { _exclusiveLock.unlockWrite(); }

/// @brief read locks a collection, with a timeout
int RocksDBMetaCollection::lockRead(double timeout) {
  uint64_t waitTime = 0;  // indicates that time is uninitialized
  double startTime = 0.0;
  
  while (true) {
    TRY_READ_LOCKER(locker, _exclusiveLock);
    
    if (locker.isLocked()) {
      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }
    
    double now = TRI_microtime();
    
    if (waitTime == 0) {  // initialize times
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = defaultLockTimeout;
      }
      
      startTime = now;
      waitTime = 1;
    }
    
    if (now > startTime + timeout) {
      LOG_TOPIC("dcbd2", TRACE, arangodb::Logger::ENGINES)
      << "timed out after " << timeout << " s waiting for read-lock on collection '"
      << _logicalCollection.name() << "'";
      
      return TRI_ERROR_LOCK_TIMEOUT;
    }
    
    if (now - startTime < 0.001) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
      
      if (waitTime < 32) {
        waitTime *= 2;
      }
    }
  }
}

/// @brief read unlocks a collection
void RocksDBMetaCollection::unlockRead() { _exclusiveLock.unlockRead(); }

void RocksDBMetaCollection::trackWaitForSync(arangodb::transaction::Methods* trx,
                                             OperationOptions& options) {
  if (_logicalCollection.waitForSync() && !options.isRestore) {
    options.waitForSync = true;
  }
  
  if (options.waitForSync) {
    trx->state()->waitForSync(true);
  }
}

// rescans the collection to update document count
uint64_t RocksDBMetaCollection::recalculateCounts() {
  RocksDBEngine* engine = rocksutils::globalRocksEngine();
  rocksdb::TransactionDB* db = engine->db();
  const rocksdb::Snapshot* snapshot = nullptr;
  // start transaction to get a collection lock
  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  if (!vocbase.use()) {  // someone dropped the database
    return _meta.numberDocuments();
  }
  auto useGuard = scopeGuard([&] {
    // cppcheck-suppress knownConditionTrueFalse
    if (snapshot) {
      db->ReleaseSnapshot(snapshot);
    }
    vocbase.release();
  });
  
  TRI_vocbase_col_status_e status;
  int res = vocbase.useCollection(&_logicalCollection, status);
  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }
  auto collGuard =
  scopeGuard([&] { vocbase.releaseCollection(&_logicalCollection); });
  
  uint64_t snapNumberOfDocuments = 0;
  {
    // fetch number docs and snapshot under exclusive lock
    // this should enable us to correct the count later
    auto lockGuard = scopeGuard([this] { unlockWrite(); });
    res = lockWrite(transaction::Options::defaultLockTimeout);
    if (res != TRI_ERROR_NO_ERROR) {
      lockGuard.cancel();
      THROW_ARANGO_EXCEPTION(res);
    }
    
    snapNumberOfDocuments = _meta.numberDocuments();
    snapshot = engine->db()->GetSnapshot();
    TRI_ASSERT(snapshot);
  }
  
  // count documents
  RocksDBKeyBounds bounds = this->bounds();
  rocksdb::Slice upper(bounds.end());
  
  rocksdb::ReadOptions ro;
  ro.snapshot = snapshot;
  ro.prefix_same_as_start = true;
  ro.iterate_upper_bound = &upper;
  ro.verify_checksums = false;
  ro.fill_cache = false;
  
  rocksdb::ColumnFamilyHandle* cf = bounds.columnFamily();
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, cf));
  std::size_t count = 0;
  
  for (it->Seek(bounds.start()); it->Valid(); it->Next()) {
    TRI_ASSERT(it->key().compare(upper) < 0);
    ++count;
  }
  
  int64_t adjustment = snapNumberOfDocuments - count;
  if (adjustment != 0) {
    LOG_TOPIC("ad6d3", WARN, Logger::REPLICATION)
    << "inconsistent collection count detected, "
    << "an offet of " << adjustment << " will be applied";
    _meta.adjustNumberDocuments(0, static_cast<TRI_voc_rid_t>(0), adjustment);
  }
  
  return _meta.numberDocuments();
}

Result RocksDBMetaCollection::compact() {
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  rocksdb::CompactRangeOptions opts;
  RocksDBKeyBounds bounds = this->bounds();
  rocksdb::Slice b = bounds.start(), e = bounds.end();
  db->CompactRange(opts, bounds.columnFamily(), &b, &e);
  
  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> i : _indexes) {
    RocksDBIndex* index = static_cast<RocksDBIndex*>(i.get());
    index->compact();
  }
  
  return {};
}

void RocksDBMetaCollection::estimateSize(velocypack::Builder& builder) {
  TRI_ASSERT(!builder.isOpenObject() && !builder.isOpenArray());
  
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  RocksDBKeyBounds bounds = this->bounds();
  rocksdb::Range r(bounds.start(), bounds.end());
  uint64_t out = 0, total = 0;
  db->GetApproximateSizes(bounds.columnFamily(), &r, 1, &out,
                          static_cast<uint8_t>(
                                               rocksdb::DB::SizeApproximationFlags::INCLUDE_MEMTABLES |
                                               rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES));
  total += out;
  
  builder.openObject();
  builder.add("documents", VPackValue(out));
  builder.add("indexes", VPackValue(VPackValueType::Object));
  
  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> i : _indexes) {
    RocksDBIndex* index = static_cast<RocksDBIndex*>(i.get());
    out = index->memory();
    builder.add(std::to_string(index->id()), VPackValue(out));
    total += out;
  }
  builder.close();
  builder.add("total", VPackValue(total));
  builder.close();
}

void RocksDBMetaCollection::setRevisionTree(std::unique_ptr<containers::RevisionTree>&& tree,
                                            uint64_t seq) {
  TRI_ASSERT(_logicalCollection.syncByRevision());
  TRI_ASSERT(tree);
  _revisionTree = std::move(tree);
  _revisionTreeApplied = seq;
}

containers::RevisionTree& RocksDBMetaCollection::revisionTree() {
  TRI_ASSERT(_logicalCollection.syncByRevision());
  TRI_ASSERT(_revisionTree);
  return *_revisionTree;
}

std::unique_ptr<containers::RevisionTree> RocksDBMetaCollection::revisionTree(transaction::Methods& trx) {
  if (!_logicalCollection.syncByRevision()) {
    return nullptr;
  }
  TRI_ASSERT(_revisionTree);

  // first apply any updates that can be safely applied
  RocksDBEngine* engine = rocksutils::globalRocksEngine();
  rocksdb::DB* db = engine->db()->GetRootDB();
  rocksdb::SequenceNumber safeSeq = meta().committableSeq(db->GetLatestSequenceNumber());
  applyUpdates(safeSeq);

  // now clone the tree so we can apply all updates consistent with our ongoing trx
  std::shared_lock<std::shared_mutex> guard(_revisionBufferLock);
  auto tree = _revisionTree->clone();
  if (!tree) {
    return nullptr;
  }

  {
    // apply any which are buffered and older than our ongoing transaction start
    rocksdb::SequenceNumber trxSeq = RocksDBTransactionState::toState(&trx)->beginSeq();
    TRI_ASSERT(trxSeq != 0);
    Result res = applyUpdatesForTransaction(*tree, trxSeq);
    if (res.fail()) {
      return nullptr;
    }
  }

  {
    // now peek at updates buffered inside transaction and apply those too
    auto operations = RocksDBTransactionState::toState(&trx)->trackedOperations(
        _logicalCollection.id());
    tree->insert(operations.inserts);
    tree->remove(operations.removals);
  }

  return tree;
}

std::unique_ptr<containers::RevisionTree> RocksDBMetaCollection::revisionTree(uint64_t batchId) {
  if (!_logicalCollection.syncByRevision()) {
    return nullptr;
  }
  TRI_ASSERT(_revisionTree);

  EngineSelectorFeature& selector =
      _logicalCollection.vocbase().server().getFeature<EngineSelectorFeature>();
  // first apply any updates that can be safely applied
  RocksDBEngine* engine = rocksutils::globalRocksEngine();
  rocksdb::DB* db = engine->db()->GetRootDB();
  rocksdb::SequenceNumber safeSeq = meta().committableSeq(db->GetLatestSequenceNumber());
  applyUpdates(safeSeq);

  // now clone the tree so we can apply all updates consistent with our ongoing trx
  std::shared_lock<std::shared_mutex> guard(_revisionBufferLock);
  auto tree = _revisionTree->clone();
  if (!tree) {
    return nullptr;
  }

  {
    // apply any which are buffered and older than our ongoing transaction start
    RocksDBEngine& engine = selector.engine<RocksDBEngine>();
    RocksDBReplicationManager* manager = engine.replicationManager();
    RocksDBReplicationContext* ctx = batchId == 0 ? nullptr : manager->find(batchId);
    if (!ctx) {
      return nullptr;
    }
    rocksdb::Snapshot const* snapshot = ctx->snapshot();
    rocksdb::SequenceNumber trxSeq = snapshot->GetSequenceNumber();
    TRI_ASSERT(trxSeq != 0);
    Result res = applyUpdatesForTransaction(*tree, trxSeq);
    if (res.fail()) {
      return nullptr;
    }
  }

  return tree;
}

Result RocksDBMetaCollection::rebuildRevisionTree() {
  std::unique_lock<std::shared_mutex> guard(_revisionBufferLock);
  _revisionTree =
      std::make_unique<containers::RevisionTree>(6, _logicalCollection.minRevision());

  Result res = basics::catchToResult([this]() -> Result {
    auto ctxt = transaction::StandaloneContext::Create(_logicalCollection.vocbase());
    SingleCollectionTransaction trx(ctxt, _logicalCollection, AccessMode::Type::READ);

    std::vector<std::size_t> revisions;
    auto iter = getReplicationIterator(ReplicationIterator::Ordering::Revision, trx);
    if (!iter) {
      LOG_TOPIC("d1e54", WARN, arangodb::Logger::ENGINES)
          << "failed to retrieve replication iterator to rebuild revision tree "
             "for collection '"
          << _logicalCollection.id() << "'";
      return Result(TRI_ERROR_INTERNAL);
    }
    RevisionReplicationIterator& it =
        *static_cast<RevisionReplicationIterator*>(iter.get());
    while (it.hasMore()) {
      revisions.emplace_back(it.revision());
      if (revisions.size() >= 5000) {  // arbitrary batch size
        _revisionTree->insert(revisions);
        revisions.clear();
      }
    }
    if (!revisions.empty()) {
      _revisionTree->insert(revisions);
    }
    return Result();
  });

  if (res.fail() && res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
    // okay, we are in recovery and can't open a transaction, so we need to
    // read the raw RocksDB data; on the plus side, we are in recovery, so we
    // are single-threaded and don't need to worry about transactions anyway

    RocksDBKeyBounds documentBounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
    rocksdb::Comparator const* cmp = RocksDBColumnFamily::documents()->GetComparator();
    rocksdb::ReadOptions ro;
    rocksdb::Slice const end = documentBounds.end();
    ro.iterate_upper_bound = &end;
    ro.fill_cache = false;

    std::vector<std::size_t> revisions;
    auto* db = rocksutils::globalRocksDB();
    auto iter = db->NewIterator(ro, documentBounds.columnFamily());
    for (iter->Seek(documentBounds.start());
         iter->Valid() && cmp->Compare(iter->key(), end) < 0; iter->Next()) {
      LocalDocumentId const docId = RocksDBKey::documentId(iter->key());
      revisions.emplace_back(docId.id());
      if (revisions.size() >= 5000) {  // arbitrary batch size
        _revisionTree->insert(revisions);
        revisions.clear();
      }
    }
    if (!revisions.empty()) {
      _revisionTree->insert(revisions);
    }
  }

  return res;
}

void RocksDBMetaCollection::revisionTreeSummary(VPackBuilder& builder) {
  if (!_logicalCollection.syncByRevision()) {
    return;
  }
  TRI_ASSERT(_revisionTree);

  std::unique_lock guard(_revisionBufferLock);
  VPackObjectBuilder obj(&builder);
  obj->add("count", VPackValue(_revisionTree->count()));
  obj->add("hash", VPackValue(_revisionTree->rootValue()));
}

void RocksDBMetaCollection::placeRevisionTreeBlocker(TRI_voc_tid_t transactionId) {
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  rocksdb::SequenceNumber preSeq = db->GetLatestSequenceNumber();
  _meta.placeBlocker(transactionId, preSeq);
}

void RocksDBMetaCollection::removeRevisionTreeBlocker(TRI_voc_tid_t transactionId) {
  _meta.removeBlocker(transactionId);
}

void RocksDBMetaCollection::bufferUpdates(rocksdb::SequenceNumber seq,
                                          std::vector<std::size_t>&& inserts,
                                          std::vector<std::size_t>&& removals) {
  if (!_logicalCollection.syncByRevision()) {
    return;
  }

  TRI_ASSERT(!inserts.empty() || !removals.empty());
  std::unique_lock guard(_revisionBufferLock);
  if (_revisionTreeApplied > seq) {
    return;
  }
  if (!inserts.empty()) {
    _revisionInsertBuffers.emplace(seq, std::move(inserts));
  }
  if (!removals.empty()) {
    _revisionRemovalBuffers.emplace(seq, std::move(removals));
  }
}

Result RocksDBMetaCollection::bufferTruncate(rocksdb::SequenceNumber seq) {
  if (!_logicalCollection.syncByRevision()) {
    return Result();
  }

  Result res = basics::catchVoidToResult([&]() -> void {
    std::unique_lock guard(_revisionBufferLock);
    if (_revisionTreeApplied > seq) {
      return;
    }
    _revisionTruncateBuffer.emplace(seq);
  });
  return res;
}

rocksdb::SequenceNumber RocksDBMetaCollection::applyUpdates(rocksdb::SequenceNumber commitSeq) {
  if (!_logicalCollection.syncByRevision()) {
    return 0;
  }
  TRI_ASSERT(_revisionTree);

  rocksdb::SequenceNumber appliedSeq = 0;
  std::unique_lock guard(_revisionBufferLock);
  Result res = basics::catchVoidToResult([&]() -> void {
    auto insertIt = _revisionInsertBuffers.begin();
    auto removeIt = _revisionRemovalBuffers.begin();

    {
      rocksdb::SequenceNumber ignoreSeq = 0;  // truncate will increase this sequence
      bool foundTruncate = false;
      // check for a truncate marker
      auto it = _revisionTruncateBuffer.begin();  // sorted ASC
      while (it != _revisionTruncateBuffer.end() && *it <= commitSeq) {
        ignoreSeq = *it;
        TRI_ASSERT(ignoreSeq != 0);
        foundTruncate = true;
        appliedSeq = std::max(appliedSeq, ignoreSeq);
        it = _revisionTruncateBuffer.erase(it);
      }
      if (foundTruncate) {
        TRI_ASSERT(ignoreSeq <= commitSeq);
        while (insertIt != _revisionInsertBuffers.end() && insertIt->first <= ignoreSeq) {
          insertIt = _revisionInsertBuffers.erase(insertIt);
        }
        while (removeIt != _revisionRemovalBuffers.end() && removeIt->first <= ignoreSeq) {
          removeIt = _revisionRemovalBuffers.erase(removeIt);
        }
        _revisionTree->clear();  // clear estimates
      }
    }

    while (true) {
      std::vector<std::size_t> inserts;
      std::vector<std::size_t> removals;
      // find out if we have buffers to apply
      {
        bool haveInserts = insertIt != _revisionInsertBuffers.end() &&
                           insertIt->first <= commitSeq;
        bool haveRemovals = removeIt != _revisionRemovalBuffers.end() &&
                            removeIt->first <= commitSeq;

        bool applyInserts =
            haveInserts && (!haveRemovals || removeIt->first >= insertIt->first);
        bool applyRemovals = haveRemovals && !applyInserts;

        // check for inserts
        if (applyInserts) {
          inserts = std::move(insertIt->second);
          appliedSeq = std::max(appliedSeq, insertIt->first);
          insertIt = _revisionInsertBuffers.erase(insertIt);
        }
        // check for removals
        if (applyRemovals) {
          removals = std::move(removeIt->second);
          appliedSeq = std::max(appliedSeq, removeIt->first);
          removeIt = _revisionRemovalBuffers.erase(removeIt);
        }
      }

      // no inserts or removals left to apply, drop out of loop
      if (inserts.empty() && removals.empty()) {
        break;
      }

      // apply inserts
      if (!inserts.empty()) {
        _revisionTree->insert(inserts);
        inserts.clear();
      }

      // apply removals
      if (!removals.empty()) {
        _revisionTree->remove(removals);
        removals.clear();
      }
    }  // </while(true)>
  });
  _revisionTreeApplied = appliedSeq;
  return appliedSeq;
}

Result RocksDBMetaCollection::applyUpdatesForTransaction(containers::RevisionTree& tree,
                                                         rocksdb::SequenceNumber commitSeq) const {
  if (!_logicalCollection.syncByRevision()) {
    return Result();
  }

  Result res = basics::catchVoidToResult([&]() -> void {
    auto insertIt = _revisionInsertBuffers.begin();
    auto removeIt = _revisionRemovalBuffers.begin();

    {
      rocksdb::SequenceNumber ignoreSeq = 0;  // truncate will increase this sequence
      bool foundTruncate = false;
      // check for a truncate marker
      auto it = _revisionTruncateBuffer.begin();  // sorted ASC
      while (it != _revisionTruncateBuffer.end() && *it <= commitSeq) {
        ignoreSeq = *it;
        TRI_ASSERT(ignoreSeq != 0);
        foundTruncate = true;
        ++it;
      }
      if (foundTruncate) {
        TRI_ASSERT(ignoreSeq <= commitSeq);
        while (insertIt != _revisionInsertBuffers.end() && insertIt->first <= ignoreSeq) {
          ++insertIt;
        }
        while (removeIt != _revisionRemovalBuffers.end() && removeIt->first <= ignoreSeq) {
          ++removeIt;
        }
        tree.clear();  // clear estimates
      }
    }

    while (true) {
      std::vector<std::size_t> const* inserts = nullptr;
      std::vector<std::size_t> const* removals = nullptr;
      // find out if we have buffers to apply
      {
        bool haveInserts = insertIt != _revisionInsertBuffers.end() &&
                           insertIt->first <= commitSeq;
        bool haveRemovals = removeIt != _revisionRemovalBuffers.end() &&
                            removeIt->first <= commitSeq;

        bool applyInserts =
            haveInserts && (!haveRemovals || removeIt->first >= insertIt->first);
        bool applyRemovals = haveRemovals && !applyInserts;

        // check for inserts
        if (applyInserts) {
          inserts = &insertIt->second;
          ++insertIt;
        }
        // check for removals
        if (applyRemovals) {
          removals = &removeIt->second;
          ++removeIt;
        }
      }

      // no inserts or removals left to apply, drop out of loop
      if (!inserts && !removals) {
        break;
      }

      // apply inserts
      if (inserts) {
        tree.insert(*inserts);
      }

      // apply removals
      if (removals) {
        tree.remove(*removals);
      }
    }  // </while(true)>
  });
  return res;
}
