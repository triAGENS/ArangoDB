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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBBuilderIndex.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/files.h"
#include "Containers/HashSet.h"
#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBBuilderIndexEE.h"
#endif
#include "RocksDBEngine/Methods/RocksDBBatchedMethods.h"
#include "RocksDBEngine/Methods/RocksDBBatchedWithIndexMethods.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <rocksdb/comparator.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <stdexcept>

using namespace arangodb;
using namespace arangodb::rocksutils;

namespace {

struct BuilderCookie : public arangodb::TransactionState::Cookie {
  // do not track removed documents twice
  ::arangodb::containers::HashSet<LocalDocumentId::BaseType> tracked;
};

constexpr size_t getBatchSize(size_t numDocsHint) noexcept {
  if (numDocsHint >= 8192) {
    return 32 * 1024 * 1024;
  } else if (numDocsHint >= 1024) {
    return 4 * 1024 * 1024;
  } else {
    return 1024 * 1024;
  }
}

}  // namespace

namespace arangodb {
Result partiallyCommitInsertions(rocksdb::WriteBatchBase& batch,
                                 rocksdb::DB* rootDB,
                                 RocksDBTransactionCollection* trxColl,
                                 std::atomic<uint64_t>& docsProcessed,
                                 RocksDBIndex& ridx, bool isForeground) {
  auto docsInBatch = batch.GetWriteBatch()->Count();
  if (docsInBatch > 0) {
    rocksdb::WriteOptions wo;
    rocksdb::Status s = rootDB->Write(wo, batch.GetWriteBatch());
    if (!s.ok()) {
      return rocksutils::convertStatus(s, rocksutils::StatusHint::index);
    }
  }
  batch.Clear();

  auto ops = trxColl->stealTrackedIndexOperations();
  if (!ops.empty()) {
    TRI_ASSERT(ridx.hasSelectivityEstimate() && ops.size() == 1);
    auto it = ops.begin();
    TRI_ASSERT(ridx.id() == it->first);

    auto* estimator = ridx.estimator();
    if (estimator != nullptr) {
      if (isForeground) {
        estimator->insert(it->second.inserts);
        estimator->remove(it->second.removals);
      } else {
        uint64_t seq = rootDB->GetLatestSequenceNumber();
        // since cuckoo estimator uses a map with seq as key we need to
        estimator->bufferUpdates(seq, std::move(it->second.inserts),
                                 std::move(it->second.removals));
      }
    }
  }

  docsProcessed.fetch_add(docsInBatch, std::memory_order_relaxed);
  return {};
}

Result fillIndexSingleThreaded(
    bool foreground, RocksDBMethods& batched, rocksdb::Options const& dbOptions,
    rocksdb::WriteBatchBase& batch, std::atomic<std::uint64_t>& docsProcessed,
    trx::BuilderTrx& trx, RocksDBIndex& ridx, rocksdb::Snapshot const* snap,
    rocksdb::DB* rootDB, std::unique_ptr<rocksdb::Iterator> it,
    std::shared_ptr<std::function<arangodb::Result(uint64_t)>> progress,
    uint64_t numDocsHint) {
  Result res;
  uint64_t numDocsWritten = 0;

  RocksDBTransactionCollection* trxColl = trx.resolveTrxCollection();

  auto rcoll = static_cast<RocksDBCollection*>(ridx.collection().getPhysical());
  auto bounds = RocksDBKeyBounds::CollectionDocuments(rcoll->objectId());
  rocksdb::Slice upper(bounds.end());

  OperationOptions options;

  for (it->Seek(bounds.start()); it->Valid(); it->Next()) {
    TRI_ASSERT(it->key().compare(upper) < 0);

    res = ridx.insert(
        trx, &batched, RocksDBKey::documentId(it->key()),
        VPackSlice(reinterpret_cast<uint8_t const*>(it->value().data())),
        options, /*performChecks*/ true);
    if (res.fail()) {
      break;
    }
    numDocsWritten++;

    if (numDocsWritten % 1024 == 0) {  // commit buffered writes

          LOG_DEVEL << __FILE__ << __LINE__;
    if (progress != nullptr) {
      (*progress)(numDocsHint*100/docsProcessed);
    }

      res = partiallyCommitInsertions(batch, rootDB, trxColl, docsProcessed,
                                      ridx, foreground);
      // cppcheck-suppress identicalConditionAfterEarlyExit
      if (res.fail()) {
        break;
      }

      if (ridx.collection().vocbase().server().isStopping()) {
        res.reset(TRI_ERROR_SHUTTING_DOWN);
        break;
      }
    }
  }

  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  if (!it->status().ok() && res.ok()) {
    res =
        rocksutils::convertStatus(it->status(), rocksutils::StatusHint::index);
  }

  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  if (res.ok()) {
    res = partiallyCommitInsertions(batch, rootDB, trxColl, docsProcessed, ridx,
                                    foreground);
  }

  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  if (res.ok()) {  // required so iresearch commits
    res = trx.commit();

    if (ridx.estimator() != nullptr) {
      ridx.estimator()->setAppliedSeq(rootDB->GetLatestSequenceNumber());
    }
  }

  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  // if an error occured drop() will be called
  LOG_TOPIC("dfa3b", DEBUG, Logger::ENGINES)
      << "snapshot captured " << numDocsWritten << " " << res.errorMessage();
  return res;
}

}  // namespace arangodb

RocksDBBuilderIndex::RocksDBBuilderIndex(
    std::shared_ptr<arangodb::RocksDBIndex> wp, uint64_t numDocsHint,
    size_t numTheads)
    : RocksDBIndex{wp->id(), wp->collection(), wp->name(), wp->fields(),
                   wp->unique(), wp->sparse(), wp->columnFamily(),
                   wp->objectId(), /*useCache*/ false,
                   /*cacheManager*/ nullptr,
                   /*engine*/
                   wp->collection()
                       .vocbase()
                       .server()
                       .getFeature<EngineSelectorFeature>()
                       .engine<RocksDBEngine>()},
      _wrapped{std::move(wp)},
      _docsProcessed{0},
      _numDocsHint{numDocsHint},
      _numThreads{
          numDocsHint > kSingleThreadThreshold
              ? std::clamp(numTheads, size_t{1}, IndexFactory::kMaxParallelism)
              : size_t{1}} {
  TRI_ASSERT(_wrapped);
}

/// @brief return a VelocyPack representation of the index
void RocksDBBuilderIndex::toVelocyPack(
    VPackBuilder& builder, std::underlying_type<Serialize>::type flags) const {
  VPackBuilder inner;
  _wrapped->toVelocyPack(inner, flags);
  TRI_ASSERT(inner.slice().isObject());
  builder.openObject();  // FIXME refactor RocksDBIndex::toVelocyPack !!
  builder.add(velocypack::ObjectIterator(inner.slice()));
  if (Index::hasFlag(flags, Index::Serialize::Internals)) {
    builder.add("_inprogress", VPackValue(true));
  }
  builder.add("documentsProcessed",
              VPackValue(_docsProcessed.load(std::memory_order_relaxed)));
  builder.close();
}

/// insert index elements into the specified write batch.
Result RocksDBBuilderIndex::insert(transaction::Methods& trx,
                                   RocksDBMethods* mthd,
                                   LocalDocumentId const& documentId,
                                   arangodb::velocypack::Slice slice,
                                   OperationOptions const& /*options*/,
                                   bool /*performChecks*/) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* ctx = dynamic_cast<::BuilderCookie*>(trx.state()->cookie(this));
#else
  auto* ctx = static_cast<::BuilderCookie*>(trx.state()->cookie(this));
#endif
  if (!ctx) {
    auto ptr = std::make_unique<::BuilderCookie>();
    ctx = ptr.get();
    trx.state()->cookie(this, std::move(ptr));
  }

  // do not track document more than once
  if (ctx->tracked.find(documentId.id()) == ctx->tracked.end()) {
    ctx->tracked.insert(documentId.id());
    RocksDBLogValue val =
        RocksDBLogValue::TrackedDocumentInsert(documentId, slice);
    mthd->PutLogData(val.slice());
  }
  return Result();  // do nothing
}

/// remove index elements and put it in the specified write batch.
Result RocksDBBuilderIndex::remove(transaction::Methods& trx,
                                   RocksDBMethods* mthd,
                                   LocalDocumentId const& documentId,
                                   arangodb::velocypack::Slice slice) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* ctx = dynamic_cast<::BuilderCookie*>(trx.state()->cookie(this));
#else
  auto* ctx = static_cast<::BuilderCookie*>(trx.state()->cookie(this));
#endif
  if (!ctx) {
    auto ptr = std::make_unique<::BuilderCookie>();
    ctx = ptr.get();
    trx.state()->cookie(this, std::move(ptr));
  }

  // do not track document more than once
  if (ctx->tracked.find(documentId.id()) == ctx->tracked.end()) {
    ctx->tracked.insert(documentId.id());
    RocksDBLogValue val =
        RocksDBLogValue::TrackedDocumentRemove(documentId, slice);
    mthd->PutLogData(val.slice());
  }
  return Result();  // do nothing
}

// fast mode assuming exclusive access locked from outside
template<bool foreground>
static arangodb::Result fillIndex(
    rocksdb::DB* rootDB, RocksDBIndex& ridx, RocksDBMethods& batched,
    rocksdb::WriteBatchBase& batch, rocksdb::Snapshot const* snap,
    std::atomic<uint64_t>& docsProcessed, bool isUnique, size_t numThreads,
    uint64_t threadBatchSize, rocksdb::Options const& dbOptions,
    std::string const& idxPath,
    std::shared_ptr<std::function<arangodb::Result(uint64_t)>> progress = nullptr,
    uint64_t numDocsHint = 0) {
  // fillindex can be non transactional, we just need to clean up
  TRI_ASSERT(rootDB != nullptr);

  auto mode =
      snap == nullptr ? AccessMode::Type::EXCLUSIVE : AccessMode::Type::WRITE;
  LogicalCollection& coll = ridx.collection();
  trx::BuilderTrx trx(transaction::StandaloneContext::Create(coll.vocbase()),
                      coll, mode);
  if (mode == AccessMode::Type::EXCLUSIVE) {
    trx.addHint(transaction::Hints::Hint::LOCK_NEVER);
  }
  trx.addHint(transaction::Hints::Hint::INDEX_CREATION);

  Result res = trx.begin();
  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  RocksDBCollection* rcoll =
      static_cast<RocksDBCollection*>(ridx.collection().getPhysical());
  auto bounds = RocksDBKeyBounds::CollectionDocuments(rcoll->objectId());
  rocksdb::Slice upper(bounds.end());

  rocksdb::ReadOptions ro(/*cksum*/ false, /*cache*/ false);
  ro.snapshot = snap;
  ro.prefix_same_as_start = true;
  ro.iterate_upper_bound = &upper;

  rocksdb::ColumnFamilyHandle* docCF = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Documents);
  std::unique_ptr<rocksdb::Iterator> it(rootDB->NewIterator(ro, docCF));

  TRI_IF_FAILURE("RocksDBBuilderIndex::fillIndex") { FATAL_ERROR_EXIT(); }
#ifdef USE_ENTERPRISE
  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  IndexFiller indexFiller(isUnique, foreground, numThreads, batched,
                          threadBatchSize, dbOptions, batch, docsProcessed, trx,
                          ridx, snap, rootDB, std::move(it), idxPath, progress);
  res = indexFiller.fillIndex();
#else
  LOG_DEVEL << __FILE__ << __LINE__ << " " << progress;
  res = fillIndexSingleThreaded(foreground, batched, dbOptions, batch,
                                docsProcessed, trx, ridx, snap, rootDB,
                                std::move(it), progress);
#endif
  return res;
}

arangodb::Result RocksDBBuilderIndex::fillIndexForeground(
    std::shared_ptr<std::function<arangodb::Result(uint64_t)>> progress) {
  RocksDBIndex* internal = _wrapped.get();
  TRI_ASSERT(internal != nullptr);

  rocksdb::Snapshot const* snap = nullptr;

  auto& selector =
      _collection.vocbase().server().getFeature<EngineSelectorFeature>();
  auto& engine = selector.engine<RocksDBEngine>();
  rocksdb::DB* db = engine.db()->GetRootDB();

  Result res;
  if (this->unique()) {
    const rocksdb::Comparator* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, getBatchSize(_numDocsHint));
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch);
    LOG_DEVEL << __FILE__ << __LINE__ << " " << progress.get();
    res = ::fillIndex<true>(
        db, *internal, methods, batch, snap, std::ref(_docsProcessed), true,
        _numThreads, this->kThreadBatchSize,
        rocksdb::Options(_engine.rocksDBOptions(), {}), _engine.idxPath());
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
    RocksDBBatchedMethods methods(&batch);
    LOG_DEVEL << __FILE__ << __LINE__ << " " << progress.get();
    res = ::fillIndex<true>(
        db, *internal, methods, batch, snap, std::ref(_docsProcessed), false,
        _numThreads, this->kThreadBatchSize,
        rocksdb::Options(_engine.rocksDBOptions(), {}), _engine.idxPath(), progress, _numDocsHint);
  }

  return res;
}

namespace {
struct ReplayHandler final : public rocksdb::WriteBatch::Handler {
  ReplayHandler(uint64_t oid, RocksDBIndex& idx, transaction::Methods& trx,
                RocksDBMethods* methods)
      : _objectId(oid), _index(idx), _trx(trx), _methods(methods) {}

  bool Continue() override {
    if (_index.collection().vocbase().server().isStopping()) {
      tmpRes.reset(TRI_ERROR_SHUTTING_DOWN);
    }
    return tmpRes.ok();
  }

  void startNewBatch(rocksdb::SequenceNumber startSequence) {
    TRI_ASSERT(_currentSequence <= startSequence);

    // starting new write batch
    _startSequence = startSequence;
    _currentSequence = startSequence;
    _startOfBatch = true;
    _lastObjectID = 0;
  }

  uint64_t endBatch() {
    _lastObjectID = 0;
    return _currentSequence;
  }

  // The default implementation of LogData does nothing.
  void LogData(const rocksdb::Slice& blob) override {
    switch (RocksDBLogValue::type(blob)) {
      case RocksDBLogType::TrackedDocumentInsert:
        if (_lastObjectID == _objectId) {
          auto pair = RocksDBLogValue::trackedDocument(blob);
          tmpRes = _index.insert(_trx, _methods, pair.first, pair.second,
                                 _options, /*performChecks*/ true);
          numInserted++;
        }
        break;

      case RocksDBLogType::TrackedDocumentRemove:
        if (_lastObjectID == _objectId) {
          auto pair = RocksDBLogValue::trackedDocument(blob);
          tmpRes = _index.remove(_trx, _methods, pair.first, pair.second);
          numRemoved++;
        }
        break;

      default:  // ignore
        _lastObjectID = 0;
        break;
    }
  }

  rocksdb::Status PutCF(uint32_t column_family_id, const rocksdb::Slice& key,
                        rocksdb::Slice const& /*value*/) override {
    incTick();
    if (column_family_id == RocksDBColumnFamilyManager::get(
                                RocksDBColumnFamilyManager::Family::Definitions)
                                ->GetID()) {
      _lastObjectID = 0;
    } else if (column_family_id ==
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Documents)
                   ->GetID()) {
      _lastObjectID = RocksDBKey::objectId(key);
    }

    return rocksdb::Status();
  }

  rocksdb::Status DeleteCF(uint32_t column_family_id,
                           const rocksdb::Slice& key) override {
    incTick();
    if (column_family_id == RocksDBColumnFamilyManager::get(
                                RocksDBColumnFamilyManager::Family::Definitions)
                                ->GetID()) {
      _lastObjectID = 0;
    } else if (column_family_id ==
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Documents)
                   ->GetID()) {
      _lastObjectID = RocksDBKey::objectId(key);
    }
    return rocksdb::Status();
  }

  rocksdb::Status SingleDeleteCF(uint32_t column_family_id,
                                 const rocksdb::Slice& key) override {
    incTick();
    if (column_family_id == RocksDBColumnFamilyManager::get(
                                RocksDBColumnFamilyManager::Family::Definitions)
                                ->GetID()) {
      _lastObjectID = 0;
    } else if (column_family_id ==
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::Documents)
                   ->GetID()) {
      _lastObjectID = RocksDBKey::objectId(key);
    }
    return rocksdb::Status();
  }

  rocksdb::Status DeleteRangeCF(uint32_t column_family_id,
                                const rocksdb::Slice& begin_key,
                                const rocksdb::Slice& end_key) override {
    incTick();  // drop and truncate may use this
    if (column_family_id == _index.columnFamily()->GetID() &&
        RocksDBKey::objectId(begin_key) == _objectId &&
        RocksDBKey::objectId(end_key) == _objectId) {
      _index.afterTruncate(_currentSequence, &_trx);
    }
    return rocksdb::Status();  // make WAL iterator happy
  }

  rocksdb::Status MarkBeginPrepare(bool = false) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkBeginPrepare() handler not defined.");
  }

  rocksdb::Status MarkEndPrepare(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkEndPrepare() handler not defined.");
  }

  rocksdb::Status MarkNoop(bool /*empty_batch*/) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status MarkRollback(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkRollbackPrepare() handler not defined.");
  }

  rocksdb::Status MarkCommit(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkCommit() handler not defined.");
  }

 private:
  // tick function that is called before each new WAL entry
  void incTick() {
    if (_startOfBatch) {
      // we are at the start of a batch. do NOT increase sequence number
      _startOfBatch = false;
    } else {
      // we are inside a batch already. now increase sequence number
      ++_currentSequence;
    }
  }

 public:
  uint64_t numInserted = 0;
  uint64_t numRemoved = 0;
  Result tmpRes;

 private:
  const uint64_t _objectId;  /// collection objectID
  RocksDBIndex& _index;      /// the index to use
  transaction::Methods& _trx;
  RocksDBMethods* _methods;  /// methods to fill
  OperationOptions const _options;

  rocksdb::SequenceNumber _startSequence = 0;
  rocksdb::SequenceNumber _currentSequence = 0;
  bool _startOfBatch = false;
  uint64_t _lastObjectID = 0;
};

Result catchup(rocksdb::DB* rootDB, RocksDBIndex& ridx, RocksDBMethods& batched,
               rocksdb::WriteBatchBase& wb, AccessMode::Type mode,
               rocksdb::SequenceNumber startingFrom,
               rocksdb::SequenceNumber& lastScannedTick, uint64_t& numScanned,
               std::function<void(uint64_t)> const& reportProgress) {
  LogicalCollection& coll = ridx.collection();
  trx::BuilderTrx trx(transaction::StandaloneContext::Create(coll.vocbase()),
                      coll, mode);
  if (mode == AccessMode::Type::EXCLUSIVE) {
    trx.addHint(transaction::Hints::Hint::LOCK_NEVER);
  }
  Result res = trx.begin();
  if (res.fail()) {
    return res;
  }

  RocksDBTransactionCollection* trxColl = trx.resolveTrxCollection();
  RocksDBCollection* rcoll =
      static_cast<RocksDBCollection*>(coll.getPhysical());

  TRI_ASSERT(rootDB != nullptr);

  ReplayHandler replay(rcoll->objectId(), ridx, trx, &batched);

  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;  // reader();
  // no need verifying the WAL contents
  rocksdb::TransactionLogIterator::ReadOptions ro(false);

  rocksdb::Status s = rootDB->GetUpdatesSince(startingFrom, &iterator, ro);
  if (!s.ok()) {
    return res.reset(convertStatus(s, rocksutils::StatusHint::wal));
  }

  auto commitLambda = [&](rocksdb::SequenceNumber seq) {
    auto docsInBatch = wb.GetWriteBatch()->Count();

    if (docsInBatch > 0) {
      rocksdb::WriteOptions wo;
      rocksdb::Status s = rootDB->Write(wo, wb.GetWriteBatch());
      if (!s.ok()) {
        res = rocksutils::convertStatus(s, rocksutils::StatusHint::index);
      }
    }
    wb.Clear();

    auto ops = trxColl->stealTrackedIndexOperations();
    if (!ops.empty()) {
      TRI_ASSERT(ridx.hasSelectivityEstimate() && ops.size() == 1);
      auto it = ops.begin();
      TRI_ASSERT(ridx.id() == it->first);
      auto* estimator = ridx.estimator();
      if (estimator != nullptr) {
        estimator->bufferUpdates(seq, std::move(it->second.inserts),
                                 std::move(it->second.removals));
      }
    }

    reportProgress(docsInBatch);
  };

  LOG_TOPIC("fa362", DEBUG, Logger::ENGINES)
      << "Scanning from " << startingFrom;

  for (; iterator->Valid(); iterator->Next()) {
    rocksdb::BatchResult batch = iterator->GetBatch();
    lastScannedTick = batch.sequence;  // start of the batch
    if (batch.sequence < startingFrom) {
      continue;  // skip
    }

    replay.startNewBatch(batch.sequence);
    s = batch.writeBatchPtr->Iterate(&replay);
    if (!s.ok()) {
      res = rocksutils::convertStatus(s);
      break;
    }
    if (replay.tmpRes.fail()) {
      res = replay.tmpRes;
      break;
    }

    commitLambda(batch.sequence);
    if (res.fail()) {
      break;
    }
    lastScannedTick = replay.endBatch();
  }

  s = iterator->status();
  // we can ignore it if we get a try again return value, because that either
  // indicates a write to another collection, or a write to this collection if
  // we are not in exclusive mode, in which case we will call catchup again
  if (!s.ok() && res.ok() && !s.IsTryAgain()) {
    LOG_TOPIC("8e3a4", WARN, Logger::ENGINES)
        << "iterator error '" << s.ToString() << "'";
    res = rocksutils::convertStatus(s);
  }

  if (res.ok()) {
    numScanned = replay.numInserted + replay.numRemoved;
    res = trx.commit();  // important for iresearch
  }

  LOG_TOPIC("5796c", DEBUG, Logger::ENGINES)
      << "WAL REPLAYED insertions: " << replay.numInserted
      << "; deletions: " << replay.numRemoved << "; lastScannedTick "
      << lastScannedTick;

  return res;
}
}  // namespace

bool RocksDBBuilderIndex::Locker::lock() {
  if (!_locked) {
    if (_collection->lockWrite() != TRI_ERROR_NO_ERROR) {
      return false;
    }
    _locked = true;
  }
  return true;
}

void RocksDBBuilderIndex::Locker::unlock() {
  if (_locked) {
    _collection->unlockWrite();
    _locked = false;
  }
}

// Background index filler task
arangodb::Result RocksDBBuilderIndex::fillIndexBackground(
    Locker& locker,
    std::shared_ptr<std::function<arangodb::Result(uint64_t)>> progress) {
  TRI_ASSERT(locker.isLocked());

  arangodb::Result res;
  RocksDBIndex* internal = _wrapped.get();
  TRI_ASSERT(internal != nullptr);

  RocksDBEngine& engine = _collection.vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine<RocksDBEngine>();
  rocksdb::DB* rootDB = engine.db()->GetRootDB();
#ifdef USE_ENTERPRISE
  // acquire ownership because it's only used until this function gets out of
  // scope
  std::unique_ptr<RocksDBFilePurgePreventer> nonPurger =
      getRocksDBFilePurgePreventer(engine);
#endif

  rocksdb::Snapshot const* snap = rootDB->GetSnapshot();
  auto scope = scopeGuard([&]() noexcept {
    if (snap) {
      rootDB->ReleaseSnapshot(snap);
    }
  });
  locker.unlock();

  // Step 1. Capture with snapshot
  rocksdb::DB* db = engine.db()->GetRootDB();
  if (internal->unique()) {
    const rocksdb::Comparator* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, 32 * 1024 * 1024);
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch);
    res = ::fillIndex<false>(
        db, *internal, methods, batch, snap, std::ref(_docsProcessed), true,
        _numThreads, kThreadBatchSize,
        rocksdb::Options(_engine.rocksDBOptions(), {}), _engine.idxPath());
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(32 * 1024 * 1024);
    RocksDBBatchedMethods methods(&batch);
    res = ::fillIndex<false>(
        db, *internal, methods, batch, snap, std::ref(_docsProcessed), false,
        _numThreads, kThreadBatchSize,
        rocksdb::Options(_engine.rocksDBOptions(), {}), _engine.idxPath(), progress, _numDocsHint);
  }

  if (res.fail()) {
    return res;
  }

  auto reportProgress = [this](uint64_t docsProcessed) {
    _docsProcessed.fetch_add(docsProcessed, std::memory_order_relaxed);
  };

  rocksdb::SequenceNumber scanFrom = snap->GetSequenceNumber();

  // Step 2. Scan the WAL for documents without lock
  int maxCatchups = 3;
  rocksdb::SequenceNumber lastScanned = 0;
  uint64_t numScanned = 0;
  do {
    lastScanned = 0;
    numScanned = 0;
    if (internal->unique()) {
      const rocksdb::Comparator* cmp =
          internal->columnFamily()->GetComparator();
      // unique index. we need to keep track of all our changes because we
      // need to avoid duplicate index keys. must therefore use a
      // WriteBatchWithIndex
      rocksdb::WriteBatchWithIndex batch(cmp, 32 * 1024 * 1024);
      RocksDBBatchedWithIndexMethods methods(engine.db(), &batch);
      res = ::catchup(db, *internal, methods, batch, AccessMode::Type::WRITE,
                      scanFrom, lastScanned, numScanned, reportProgress);
    } else {
      // non-unique index. all index keys will be unique anyway because they
      // contain the document id we can therefore get away with a cheap
      // WriteBatch
      rocksdb::WriteBatch batch(32 * 1024 * 1024);
      RocksDBBatchedMethods methods(&batch);
      res = ::catchup(db, *internal, methods, batch, AccessMode::Type::WRITE,
                      scanFrom, lastScanned, numScanned, reportProgress);
    }

    if (res.fail() && !res.is(TRI_ERROR_ARANGO_TRY_AGAIN)) {
      return res;
    }

    scanFrom = lastScanned;
  } while (maxCatchups-- > 0 && numScanned > 5000);

  if (!locker.lock()) {  // acquire exclusive collection lock
    return res.reset(TRI_ERROR_LOCK_TIMEOUT);
  }

  // Step 3. Scan the WAL for documents with a lock

  scanFrom = lastScanned;
  if (internal->unique()) {
    const rocksdb::Comparator* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, 32 * 1024 * 1024);
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch);
    res = ::catchup(db, *internal, methods, batch, AccessMode::Type::EXCLUSIVE,
                    scanFrom, lastScanned, numScanned, reportProgress);
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(32 * 1024 * 1024);
    RocksDBBatchedMethods methods(&batch);
    res = ::catchup(db, *internal, methods, batch, AccessMode::Type::EXCLUSIVE,
                    scanFrom, lastScanned, numScanned, reportProgress);
  }

  return res;
}
