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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBBuilderIndex.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Basics/files.h"
#include "Containers/HashSet.h"
#include "RocksDBEngine/RocksDBFormat.h"
#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBBuilderIndexEE.h"
#endif
#include "Logger/LogMacros.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/FlushFeature.h"
#include "RocksDBEngine/Methods/RocksDBBatchedBaseMethods.h"
#include "RocksDBEngine/Methods/RocksDBBatchedMethods.h"
#include "RocksDBEngine/Methods/RocksDBBatchedWithIndexMethods.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBMethodsMemoryTracker.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "Statistics/ServerStatistics.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "RocksDBVectorIndex.h"

#include <absl/strings/str_cat.h>

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

constexpr size_t getBatchSize(size_t numDocsHint) noexcept {
  if (numDocsHint >= 8192) {
    return 16 * 1024 * 1024;
  } else if (numDocsHint >= 1024) {
    return 4 * 1024 * 1024;
  } else {
    return 1024 * 1024;
  }
}

}  // namespace

namespace arangodb {
Result partiallyCommitInsertions(RocksDBBatchedBaseMethods& batched,
                                 rocksdb::WriteBatchBase& batch,
                                 rocksdb::DB* rootDB,
                                 RocksDBTransactionCollection* trxColl,
                                 std::atomic<uint64_t>& docsProcessed,
                                 RocksDBIndex& ridx, bool isForeground) {
  TRI_ASSERT(rootDB != nullptr);
  TRI_ASSERT(trxColl != nullptr);

  auto docsInBatch = batch.GetWriteBatch()->Count();
  if (docsInBatch > 0) {
    rocksdb::WriteOptions wo;
    rocksdb::Status s = rootDB->Write(wo, batch.GetWriteBatch());
    if (!s.ok()) {
      return rocksutils::convertStatus(s, rocksutils::StatusHint::index);
    }
  }
  batch.Clear();
  batched.resetMemoryUsage();

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
    bool foreground, RocksDBBatchedBaseMethods& batched,
    rocksdb::Options const& dbOptions, rocksdb::WriteBatchBase& batch,
    std::atomic<std::uint64_t>& docsProcessed, trx::BuilderTrx& trx,
    RocksDBIndex& ridx, rocksdb::Snapshot const* snap, rocksdb::DB* rootDB,
    std::unique_ptr<rocksdb::Iterator> it,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress) {
  Result res;
  uint64_t numDocsWritten = 0;

  RocksDBTransactionCollection* trxColl = trx.resolveTrxCollection();

  auto rcoll = static_cast<RocksDBCollection*>(ridx.collection().getPhysical());
  auto bounds = RocksDBKeyBounds::CollectionDocuments(rcoll->objectId());
  auto count = rcoll->numberDocuments(&trx);
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

    if (numDocsWritten > 0 &&
        numDocsWritten % 1024 == 0) {  // commit buffered writes

      res = partiallyCommitInsertions(batched, batch, rootDB, trxColl,
                                      docsProcessed, ridx, foreground);

      // here come our 13 reasons why we may want to abort the index creation...

      // cppcheck-suppress identicalConditionAfterEarlyExit
      if (res.fail()) {
        break;
      }

      if (ridx.collection().vocbase().server().isStopping()) {
        res.reset(TRI_ERROR_SHUTTING_DOWN);
        break;
      }
      if (ridx.collection().vocbase().isDropped()) {
        // database dropped
        res.reset(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
        break;
      }
      if (ridx.collection().deleted()) {
        // collection dropped
        res.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
        break;
      }

      if (count > 0) {
        double p =
            docsProcessed.load(std::memory_order_relaxed) * 100.0 / count;
        ridx.progress(p);
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
        TRI_IF_FAILURE("fillIndex::pause") {
          while (true) {
            TRI_IF_FAILURE("fillIndex::unpause") { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }
#endif
        if (progress != nullptr) {
          (*progress)(p);
        }
      }
    }
  }

  if (!it->status().ok() && res.ok()) {
    res =
        rocksutils::convertStatus(it->status(), rocksutils::StatusHint::index);
  }

  if (res.ok()) {
    res = partiallyCommitInsertions(batched, batch, rootDB, trxColl,
                                    docsProcessed, ridx, foreground);
  }

  if (res.ok()) {  // required so iresearch commits
    res = trx.commit();

    ridx.progress(100.0);  // Report ready
    if (progress != nullptr) {
      (*progress)(100.0);
    }
    if (ridx.estimator() != nullptr) {
      ridx.estimator()->setAppliedSeq(rootDB->GetLatestSequenceNumber());
    }
  }

  // if an error occured drop() will be called
  LOG_TOPIC("dfa3b", DEBUG, Logger::ENGINES)
      << "snapshot captured " << numDocsWritten << " " << res.errorMessage();
  return res;
}

}  // namespace arangodb

RocksDBBuilderIndex::RocksDBBuilderIndex(std::shared_ptr<RocksDBIndex> wp,
                                         uint64_t numDocsHint,
                                         size_t numThreads)
    : RocksDBIndex{wp->id(), wp->collection(), wp->name(), wp->fields(),
                   wp->unique(), wp->sparse(), wp->columnFamily(),
                   wp->objectId(), /*useCache*/ false,
                   /*cacheManager*/ nullptr,
                   /*engine*/
                   static_cast<RocksDBEngine&>(
                       wp->collection().vocbase().engine())},
      _wrapped{std::move(wp)},
      _docsProcessed{0},
      _numDocsHint{numDocsHint},
      _numThreads{
          numDocsHint > kSingleThreadThreshold
              ? std::clamp(numThreads, size_t{1}, IndexFactory::kMaxParallelism)
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
                                   LocalDocumentId documentId,
                                   velocypack::Slice slice,
                                   OperationOptions const& /*options*/,
                                   bool /*performChecks*/) {
  RocksDBLogValue val =
      RocksDBLogValue::TrackedDocumentInsert(documentId, slice);
  mthd->PutLogData(val.slice());
  return {};  // do nothing
}

/// remove index elements and put it in the specified write batch.
Result RocksDBBuilderIndex::remove(transaction::Methods& trx,
                                   RocksDBMethods* mthd,
                                   LocalDocumentId documentId,
                                   velocypack::Slice slice,
                                   OperationOptions const& /*options*/) {
  RocksDBLogValue val =
      RocksDBLogValue::TrackedDocumentRemove(documentId, slice);
  mthd->PutLogData(val.slice());
  return {};  // do nothing
}

// fast mode assuming exclusive access locked from outside
template<bool foreground>
static Result fillIndex(
    rocksdb::DB* rootDB, RocksDBIndex& ridx, RocksDBBatchedBaseMethods& batched,
    rocksdb::WriteBatchBase& batch, rocksdb::Snapshot const* snap,
    std::atomic<uint64_t>& docsProcessed, bool isUnique, size_t numThreads,
    uint64_t threadBatchSize, rocksdb::Options const& dbOptions,
    std::string const& idxPath,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress =
        nullptr) {
  // fillindex can be non transactional, we just need to clean up
  TRI_ASSERT(rootDB != nullptr);

  auto mode =
      snap == nullptr ? AccessMode::Type::EXCLUSIVE : AccessMode::Type::WRITE;
  LogicalCollection& coll = ridx.collection();
  transaction::Options trxOpts;
  trxOpts.requiresReplication = false;
  auto origin = transaction::OperationOriginREST{"building index"};
  trx::BuilderTrx trx(
      transaction::StandaloneContext::create(coll.vocbase(), origin), coll,
      mode, trxOpts);
  if (mode == AccessMode::Type::EXCLUSIVE) {
    trx.addHint(transaction::Hints::Hint::LOCK_NEVER);
  }
  // When we begin the trx on Replication2 we try to load the leader state.
  // This behavior is not desired for index creation on the follower, as it
  // will result in an error. Using this hint we prevent loading the
  // leaderState.
  // The same is for background indexes.
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

  if (ridx.type() == arangodb::Index::TRI_IDX_TYPE_VECTOR_INDEX) {
    it->Seek(bounds.start());
    return dynamic_cast<RocksDBVectorIndex&>(ridx).ingestVectors(rootDB,
                                                                 std::move(it));
  }

  TRI_IF_FAILURE("RocksDBBuilderIndex::fillIndex") { FATAL_ERROR_EXIT(); }
#ifdef USE_ENTERPRISE
  if (arangodb::rocksutils::rocksDBEndianness == RocksDBEndianness::Little &&
      numThreads > 1) {
    LOG_TOPIC("e3241", WARN, Logger::ENGINES)
        << "Number of threads for index generation forced from " << numThreads
        << " down to 1 since the RocksDB internal data format is still "
        << "little endian keys!";
    numThreads = 1;
  }

  IndexFiller indexFiller(isUnique, foreground, numThreads, batched,
                          threadBatchSize, dbOptions, batch, docsProcessed, trx,
                          ridx, snap, rootDB, std::move(it), idxPath,
                          std::move(progress));
  res = indexFiller.fillIndex();
#else
  res = fillIndexSingleThreaded(foreground, batched, dbOptions, batch,
                                docsProcessed, trx, ridx, snap, rootDB,
                                std::move(it), std::move(progress));
#endif
  return res;
}

void RocksDBBuilderIndex::beforeCreate() {
  RocksDBIndex* internal = _wrapped.get();
  TRI_ASSERT(_wrapped.get() != nullptr);
  rocksdb::Snapshot const* snap = nullptr;

  auto& engine = static_cast<RocksDBEngine&>(_collection.vocbase().engine());
  rocksdb::DB* db = engine.db()->GetRootDB();

  auto& metric = _collection.vocbase()
                     .server()
                     .getFeature<metrics::MetricsFeature>()
                     .serverStatistics()
                     ._transactionsStatistics._restTransactionsMemoryUsage;
  RocksDBMethodsMemoryTracker memoryTracker(
      nullptr, &metric,
      /*granularity*/ RocksDBMethodsMemoryTracker::kDefaultGranularity);

  rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
  RocksDBBatchedMethods methods(&batch, memoryTracker);

  // From fillIndex
  auto const mode =
      snap == nullptr ? AccessMode::Type::EXCLUSIVE : AccessMode::Type::WRITE;
  LogicalCollection const& coll = internal->collection();
  transaction::Options trxOpts;
  trxOpts.requiresReplication = false;
  auto const origin = transaction::OperationOriginREST{"preparing index"};
  trx::BuilderTrx trx(
      transaction::StandaloneContext::create(coll.vocbase(), origin), coll,
      mode, trxOpts);
  if (mode == AccessMode::Type::EXCLUSIVE) {
    trx.addHint(transaction::Hints::Hint::LOCK_NEVER);
  }
  trx.addHint(transaction::Hints::Hint::INDEX_CREATION);

  auto const* rcoll = static_cast<RocksDBCollection const*>(
      internal->collection().getPhysical());
  auto const bounds = RocksDBKeyBounds::CollectionDocuments(rcoll->objectId());
  rocksdb::Slice upper(bounds.end());

  rocksdb::ReadOptions ro(/*cksum*/ false, /*cache*/ false);
  ro.snapshot = snap;
  ro.prefix_same_as_start = true;
  ro.iterate_upper_bound = &upper;

  rocksdb::ColumnFamilyHandle* docCF = RocksDBColumnFamilyManager::get(
      RocksDBColumnFamilyManager::Family::Documents);
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, docCF));

  it->Seek(bounds.start());
  _wrapped->prepareIndex(std::move(it), upper, &methods);
}

Result RocksDBBuilderIndex::fillIndexForeground(
    std::shared_ptr<std::function<arangodb::Result(double)>> progress) {
  RocksDBIndex* internal = _wrapped.get();
  TRI_ASSERT(internal != nullptr);

  rocksdb::Snapshot const* snap = nullptr;

  auto& engine = static_cast<RocksDBEngine&>(_collection.vocbase().engine());
  rocksdb::DB* db = engine.db()->GetRootDB();

  auto& metric = _collection.vocbase()
                     .server()
                     .getFeature<metrics::MetricsFeature>()
                     .serverStatistics()
                     ._transactionsStatistics._restTransactionsMemoryUsage;
  RocksDBMethodsMemoryTracker memoryTracker(
      nullptr, &metric,
      /*granularity*/ RocksDBMethodsMemoryTracker::kDefaultGranularity);

  Result res;
  if (this->unique()) {
    rocksdb::Comparator const* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, getBatchSize(_numDocsHint));
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch, memoryTracker);
    res = ::fillIndex<true>(
        db, *internal, methods, batch, snap, std::ref(_docsProcessed), true,
        _numThreads, this->kThreadBatchSize,
        rocksdb::Options(_engine.rocksDBOptions(), {}), _engine.idxPath());
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
    RocksDBBatchedMethods methods(&batch, memoryTracker);
    res = ::fillIndex<true>(db, *internal, methods, batch, snap,
                            std::ref(_docsProcessed), false, _numThreads,
                            this->kThreadBatchSize,
                            rocksdb::Options(_engine.rocksDBOptions(), {}),
                            _engine.idxPath(), std::move(progress));
  }

  return res;
}

namespace {

class LowerBoundTracker final : public FlushSubscription {
 public:
  explicit LowerBoundTracker(TRI_voc_tick_t tick, std::string const& name)
      : _tick{tick}, _name{name} {}

  /// @brief earliest tick that can be released
  [[nodiscard]] TRI_voc_tick_t tick() const noexcept final {
    return _tick.load(std::memory_order_acquire);
  }

  void tick(TRI_voc_tick_t tick) noexcept {
    auto value = _tick.load(std::memory_order_acquire);
    TRI_ASSERT(value <= tick);

    // tick value must never go backwards
    while (tick > value) {
      if (_tick.compare_exchange_weak(value, tick, std::memory_order_release,
                                      std::memory_order_acquire)) {
        break;
      }
    }
  }

  std::string const& name() const final { return _name; }

 private:
  std::atomic<TRI_voc_tick_t> _tick;
  std::string const _name;
};

struct ReplayHandler final : public rocksdb::WriteBatch::Handler {
  ReplayHandler(uint64_t oid, RocksDBIndex& idx, transaction::Methods& trx,
                RocksDBMethods* methods)
      : _objectId(oid), _index(idx), _trx(trx), _methods(methods) {}

  bool Continue() override {
    if (_index.collection().vocbase().server().isStopping()) {
      tmpRes.reset(TRI_ERROR_SHUTTING_DOWN);
    }
    if (++_iterations % 128 == 0) {
      // check every now and then if we can abort replaying
      if (_index.collection().vocbase().isDropped()) {
        // database dropped
        tmpRes.reset(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
      } else if (_index.collection().deleted()) {
        // collection dropped
        tmpRes.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      }
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
          tmpRes =
              _index.remove(_trx, _methods, pair.first, pair.second, _options);
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
      _index.truncateCommit({}, _currentSequence, &_trx);
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
  uint64_t const _objectId;  /// collection objectID
  RocksDBIndex& _index;      /// the index to use
  transaction::Methods& _trx;
  RocksDBMethods* _methods;  /// methods to fill
  OperationOptions const _options;

  rocksdb::SequenceNumber _startSequence = 0;
  rocksdb::SequenceNumber _currentSequence = 0;
  bool _startOfBatch = false;
  uint64_t _lastObjectID = 0;
  uint64_t _iterations = 0;
};

Result catchup(rocksdb::DB* rootDB, RocksDBIndex& ridx,
               RocksDBBatchedBaseMethods& batched, rocksdb::WriteBatchBase& wb,
               AccessMode::Type mode, rocksdb::SequenceNumber startingFrom,
               rocksdb::SequenceNumber& lastScannedTick, uint64_t& numScanned,
               std::function<void(uint64_t)> const& reportProgress,
               LowerBoundTracker& lowerBoundTracker) {
  // push forward WAL lower bound tick
  lowerBoundTracker.tick(startingFrom);

  LogicalCollection& coll = ridx.collection();
  auto origin = transaction::OperationOriginREST{"building index"};
  trx::BuilderTrx trx(
      transaction::StandaloneContext::create(coll.vocbase(), origin), coll,
      mode);
  if (mode == AccessMode::Type::EXCLUSIVE) {
    trx.addHint(transaction::Hints::Hint::LOCK_NEVER);
  }
  // When we begin the trx on Replication2 we try to load the leader state.
  // This behavior is not desired for index creation on the follower, as it
  // will result in an error. Using this hint we prevent loading the
  // leaderState.
  // The same is for foreground indexes.
  trx.addHint(transaction::Hints::Hint::INDEX_CREATION);
  Result res = trx.begin();
  if (res.fail()) {
    return res;
  }

  RocksDBTransactionCollection* trxColl = trx.resolveTrxCollection();
  RocksDBCollection* rcoll =
      static_cast<RocksDBCollection*>(coll.getPhysical());

  TRI_ASSERT(rootDB != nullptr);

  ReplayHandler replay(rcoll->objectId(), ridx, trx, &batched);

  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;
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
    batched.resetMemoryUsage();

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

  lastScannedTick = startingFrom;

  for (; iterator->Valid(); iterator->Next()) {
    rocksdb::BatchResult batch = iterator->GetBatch();
    if (batch.sequence < startingFrom) {
      continue;  // skip
    }

    lastScannedTick = batch.sequence;  // start of the batch

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

    lowerBoundTracker.tick(batch.sequence);
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

futures::Future<bool> RocksDBBuilderIndex::Locker::lock() {
  if (!_locked) {
    if (co_await _collection->lockWrite() != TRI_ERROR_NO_ERROR) {
      co_return false;
    }
    _locked = true;
  }
  co_return true;
}

void RocksDBBuilderIndex::Locker::unlock() {
  if (_locked) {
    _collection->unlockWrite();
    _locked = false;
  }
}

// Background index filler task
futures::Future<Result> RocksDBBuilderIndex::fillIndexBackground(
    Locker& locker,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress) {
  TRI_ASSERT(locker.isLocked());

  RocksDBIndex* internal = _wrapped.get();
  TRI_ASSERT(internal != nullptr);

  RocksDBEngine& engine = _collection.vocbase().engine<RocksDBEngine>();
  rocksdb::DB* rootDB = engine.db()->GetRootDB();

  rocksdb::Snapshot const* snap = rootDB->GetSnapshot();
  auto scope = scopeGuard([&]() noexcept {
    if (snap) {
      rootDB->ReleaseSnapshot(snap);
    }
  });

  std::string name =
      absl::StrCat("index creation for ", _collection.vocbase().name(), "/",
                   _collection.name());

  // prevent WAL deletion from this tick
  auto lowerBoundTracker =
      std::make_shared<LowerBoundTracker>(snap->GetSequenceNumber(), name);
  auto& flushFeature =
      _collection.vocbase().server().getFeature<FlushFeature>();
  flushFeature.registerFlushSubscription(lowerBoundTracker);

  locker.unlock();

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  while (TRI_ShouldFailDebugging("BuilderIndex::purgeWal")) {
    engine.pruneWalFiles();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
#endif

  auto& metric = _collection.vocbase()
                     .server()
                     .getFeature<metrics::MetricsFeature>()
                     .serverStatistics()
                     ._transactionsStatistics._restTransactionsMemoryUsage;
  RocksDBMethodsMemoryTracker memoryTracker(
      nullptr, &metric,
      /*granularity*/ RocksDBMethodsMemoryTracker::kDefaultGranularity);

  Result res;
  // Step 1. Capture with snapshot
  rocksdb::DB* db = engine.db()->GetRootDB();
  if (internal->unique()) {
    rocksdb::Comparator const* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, getBatchSize(_numDocsHint));
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch, memoryTracker);
    res = ::fillIndex<false>(db, *internal, methods, batch, snap,
                             std::ref(_docsProcessed), true, _numThreads,
                             kThreadBatchSize,
                             rocksdb::Options(_engine.rocksDBOptions(), {}),
                             _engine.idxPath(), std::move(progress));
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
    RocksDBBatchedMethods methods(&batch, memoryTracker);
    res = ::fillIndex<false>(db, *internal, methods, batch, snap,
                             std::ref(_docsProcessed), false, _numThreads,
                             kThreadBatchSize,
                             rocksdb::Options(_engine.rocksDBOptions(), {}),
                             _engine.idxPath(), std::move(progress));
  }

  if (res.fail()) {
    co_return res;
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
      rocksdb::Comparator const* cmp =
          internal->columnFamily()->GetComparator();
      // unique index. we need to keep track of all our changes because we
      // need to avoid duplicate index keys. must therefore use a
      // WriteBatchWithIndex
      rocksdb::WriteBatchWithIndex batch(cmp, getBatchSize(_numDocsHint));
      RocksDBBatchedWithIndexMethods methods(engine.db(), &batch,
                                             memoryTracker);
      res = ::catchup(db, *internal, methods, batch, AccessMode::Type::WRITE,
                      scanFrom, lastScanned, numScanned, reportProgress,
                      *lowerBoundTracker);
    } else {
      // non-unique index. all index keys will be unique anyway because they
      // contain the document id we can therefore get away with a cheap
      // WriteBatch
      rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
      RocksDBBatchedMethods methods(&batch, memoryTracker);
      res = ::catchup(db, *internal, methods, batch, AccessMode::Type::WRITE,
                      scanFrom, lastScanned, numScanned, reportProgress,
                      *lowerBoundTracker);
    }

    if (res.fail() && !res.is(TRI_ERROR_ARANGO_TRY_AGAIN)) {
      co_return res;
    }

    scanFrom = lastScanned;
  } while (maxCatchups-- > 0 && numScanned > 5000);

  if (!co_await locker.lock()) {  // acquire exclusive collection lock
    co_return res.reset(TRI_ERROR_LOCK_TIMEOUT);
  }

  // Step 3. Scan the WAL for documents with a lock

  scanFrom = lastScanned;
  if (internal->unique()) {
    rocksdb::Comparator const* cmp = internal->columnFamily()->GetComparator();
    // unique index. we need to keep track of all our changes because we need
    // to avoid duplicate index keys. must therefore use a WriteBatchWithIndex
    rocksdb::WriteBatchWithIndex batch(cmp, getBatchSize(_numDocsHint));
    RocksDBBatchedWithIndexMethods methods(engine.db(), &batch, memoryTracker);
    res = ::catchup(db, *internal, methods, batch, AccessMode::Type::EXCLUSIVE,
                    scanFrom, lastScanned, numScanned, reportProgress,
                    *lowerBoundTracker);
  } else {
    // non-unique index. all index keys will be unique anyway because they
    // contain the document id we can therefore get away with a cheap
    // WriteBatch
    rocksdb::WriteBatch batch(getBatchSize(_numDocsHint));
    RocksDBBatchedMethods methods(&batch, memoryTracker);
    res = ::catchup(db, *internal, methods, batch, AccessMode::Type::EXCLUSIVE,
                    scanFrom, lastScanned, numScanned, reportProgress,
                    *lowerBoundTracker);
  }

  co_return res;
}
