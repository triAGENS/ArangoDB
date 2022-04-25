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

#pragma once

#include "Basics/ConditionLocker.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace trx {
struct BuilderTrx : public arangodb::transaction::Methods {
  BuilderTrx(
      std::shared_ptr<arangodb::transaction::Context> const& transactionContext,
      arangodb::LogicalDataSource const& collection,
      arangodb::AccessMode::Type type)
      : arangodb::transaction::Methods(transactionContext),
        _cid(collection.id()) {
    // add the (sole) data-source
    addCollection(collection.id(), collection.name(), type);
    addHint(arangodb::transaction::Hints::Hint::NO_DLD);
  }

  /// @brief get the underlying transaction collection
  arangodb::RocksDBTransactionCollection* resolveTrxCollection() {
    return static_cast<arangodb::RocksDBTransactionCollection*>(
        trxCollection(_cid));
  }

 private:
  arangodb::DataSourceId _cid;
};
}  // namespace trx

namespace arangodb {

class SharedWorkEnv;

class RocksDBCollection;

class IndexCreatorThread final : public Thread {
 public:
  IndexCreatorThread(bool isUniqueIndex, bool isForeground,
                     uint64_t lastDocIdInRange,
                     std::atomic<uint64_t>& docsProcessed,
                     std::shared_ptr<SharedWorkEnv> sharedWorkEnv,
                     RocksDBCollection* rcoll, rocksdb::DB* rootDB,
                     RocksDBIndex& ridx, trx::BuilderTrx& trx);

  ~IndexCreatorThread() { Thread::shutdown(); }

  void run() override;

  Result commitInsertions();

 private:
  bool _isUniqueIndex = false;
  bool _isForeground = false;
  uint64_t const _lastDocIdInRange;
  std::atomic<uint64_t>& _docsProcessed;
  std::shared_ptr<SharedWorkEnv> _sharedWorkEnv;
  RocksDBCollection* _rcoll;
  rocksdb::DB* _rootDB;
  RocksDBIndex& _ridx;
  trx::BuilderTrx& _trx;
  RocksDBTransactionCollection* _trxColl;

  // ptrs because of abstract class, have to know which type to craete
  std::unique_ptr<rocksdb::WriteBatchBase> _batch;
  std::unique_ptr<RocksDBMethods> _methods;
  rocksdb::ReadOptions _readOptions;
};

/// Dummy index class that contains the logic to build indexes
/// without an exclusive lock. It wraps the actual index implementation
/// and adds some required synchronization logic on top
class RocksDBBuilderIndex final : public arangodb::RocksDBIndex {
 public:
  explicit RocksDBBuilderIndex(std::shared_ptr<arangodb::RocksDBIndex>,
                               uint64_t numDocsHint);

  /// @brief return a VelocyPack representation of the index
  void toVelocyPack(
      velocypack::Builder& builder,
      std::underlying_type<Index::Serialize>::type) const override;

  char const* typeName() const override { return _wrapped->typeName(); }

  IndexType type() const override { return _wrapped->type(); }

  bool canBeDropped() const override {
    return false;  // TODO ?!
  }

  /// @brief whether or not the index is sorted
  bool isSorted() const override { return _wrapped->isSorted(); }

  /// @brief if true this index should not be shown externally
  bool isHidden() const override {
    return true;  // do not show building indexes
  }

  bool inProgress() const override {
    return true;  // do not show building indices
  }

  size_t memory() const override { return _wrapped->memory(); }

  Result drop() override { return _wrapped->drop(); }

  void afterTruncate(TRI_voc_tick_t tick,
                     arangodb::transaction::Methods* trx) override {
    _wrapped->afterTruncate(tick, trx);
  }

  void load() override { _wrapped->load(); }

  void unload() override { _wrapped->unload(); }

  /// @brief whether or not the index has a selectivity estimate
  bool hasSelectivityEstimate() const override { return false; }

  /// insert index elements into the specified write batch.
  Result insert(transaction::Methods& trx, RocksDBMethods*,
                LocalDocumentId const& documentId,
                arangodb::velocypack::Slice slice,
                OperationOptions const& options,
                bool /*performChecks*/) override;

  /// remove index elements and put it in the specified write batch.
  Result remove(transaction::Methods& trx, RocksDBMethods*,
                LocalDocumentId const& documentId,
                arangodb::velocypack::Slice slice) override;

  /// @brief get index estimator, optional
  RocksDBCuckooIndexEstimatorType* estimator() override {
    return _wrapped->estimator();
  }
  void setEstimator(std::unique_ptr<RocksDBCuckooIndexEstimatorType>) override {
    TRI_ASSERT(false);
  }
  void recalculateEstimates() override { _wrapped->recalculateEstimates(); }

  /// @brief assumes an exclusive lock on the collection
  Result fillIndexForeground();

  struct Locker {
    explicit Locker(RocksDBCollection* c) : _collection(c), _locked(false) {}
    ~Locker() { unlock(); }
    bool lock();
    void unlock();
    bool isLocked() const { return _locked; }

   private:
    RocksDBCollection* const _collection;
    bool _locked;
  };

  /// @brief fill the index, assume already locked exclusively
  /// @param locker locks and unlocks the collection
  Result fillIndexBackground(Locker& locker);

 private:
  std::shared_ptr<arangodb::RocksDBIndex> _wrapped;
  std::uint64_t _numDocsHint;
  std::atomic<uint64_t> _docsProcessed;
};

using WorkItem = std::pair<uint64_t, uint64_t>;
class SharedWorkEnv {
  // will change here for std::condition_variable
 public:
  SharedWorkEnv(std::deque<WorkItem>& workItems)
      : _ranges(std::move(workItems)) {}
  void markAsDone() {
    std::unique_lock<std::mutex> lock(mtx);
    _done = true;
  }

  Result result() {
    std::unique_lock<std::mutex> lock(mtx);
    return _res;
  }

  void registerError(Result res) {
    TRI_ASSERT(res.fail());
    {
      std::unique_lock<std::mutex> lock(mtx);
      if (_res.ok()) {
        _res = std::move(res);
      }
      _done = true;
    }
    _condition.notify_all();
  }

  bool fetchWorkItem(WorkItem& data) {
    std::unique_lock<std::mutex> lock(mtx);
    if (_ranges.empty()) {
      return false;
    }
    auto const it = _ranges.begin();
    data = *it;
    _ranges.pop_front();
    return true;
  }

  void enqueueWorkItem(WorkItem item) {
    {
      std::unique_lock<std::mutex> lock(mtx);
      _ranges.emplace_back(std::move(item));
    }
    _condition.notify_one();
  }

  void waitForWork() {
    std::unique_lock<std::mutex> lock(mtx);
    _condition.wait(lock);
  }

  bool shouldStop() {
    std::unique_lock<std::mutex> lock(mtx);
    return _done;
  }

  void incTerminatedThreads() {
    std::unique_lock<std::mutex> lock(mtx);
    ++_numTerminatedThreads;
  }

  size_t getNumTerminatedThreads() { return _numTerminatedThreads; }

 private:
  std::condition_variable _condition;
  std::deque<WorkItem> _ranges;
  Result _res;
  bool _done = false;
  std::atomic<size_t> _numTerminatedThreads = 0;
  std::mutex mtx;
};
}  // namespace arangodb
