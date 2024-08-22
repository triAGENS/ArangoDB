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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#include "StorageManager.h"

#include <Futures/Future.h>

#include <utility>

#include "Basics/Guarded.h"
#include "Inspection/VPack.h"
#include "Logger/LogContextKeys.h"
#include "Replication2/IScheduler.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/Storage/IStorageEngineMethods.h"

using namespace arangodb::replication2::replicated_state;

namespace arangodb::replication2::replicated_log {

struct comp::StorageManager::StorageOperation {
  virtual ~StorageOperation() = default;
  virtual auto run(IStorageEngineMethods& methods) noexcept
      -> futures::Future<Result> = 0;
};

struct comp::StorageManagerTransaction : IStorageTransaction {
  using GuardType = Guarded<StorageManager::GuardedData>::mutex_guard_type;

  auto getLogBounds() const noexcept -> LogRange override {
    return guard->spearheadMapping.getIndexRange();
  }

  template<typename F>
  auto scheduleOperation(TermIndexMapping mapResult, F&& fn) noexcept {
    return manager.scheduleOperationLambda(
        std::move(guard), std::move(mapResult), std::forward<F>(fn));
  }

  auto removeFront(LogIndex stop) noexcept -> futures::Future<Result> override {
    LOG_CTX("37d15", TRACE, manager.loggerContext)
        << "scheduling remove front, stop = " << stop;
    auto mapping = guard->spearheadMapping;
    mapping.removeFront(stop);
    return scheduleOperation(
        std::move(mapping),
        [stop](StorageManager::IStorageEngineMethods& methods) noexcept {
          return methods.removeFront(stop, {}).thenValue(
              [](auto&& res) { return res.result(); });
        });
  }

  auto removeBack(LogIndex start) noexcept -> futures::Future<Result> override {
    LOG_CTX("eb9da", TRACE, manager.loggerContext)
        << "scheduling remove back, start = " << start;
    auto mapping = guard->spearheadMapping;
    mapping.removeBack(start);
    return scheduleOperation(
        std::move(mapping),
        [start](StorageManager::IStorageEngineMethods& methods) noexcept {
          return methods.removeBack(start, {}).thenValue(
              [](auto&& res) { return res.result(); });
        });
  }

  auto appendEntries(
      InMemoryLog slice,
      storage::IStorageEngineMethods::WriteOptions writeOptions) noexcept
      -> futures::Future<Result> override {
    LOG_CTX("eb8da", TRACE, manager.loggerContext)
        << "scheduling append, range = " << slice.getIndexRange();
    ADB_PROD_ASSERT(guard->spearheadMapping.empty() ||
                    slice.getFirstIndex() ==
                        guard->spearheadMapping.getLastIndex()->index + 1)
        << "tried to append non matching slice - log range is: "
        << guard->spearheadMapping.getIndexRange() << " new piece starts at "
        << slice.getFirstIndex();
    auto iter = slice.getLogIterator();
    auto mapping = guard->spearheadMapping;
    auto sliceMapping = slice.computeTermIndexMap();
    mapping.append(sliceMapping);
    return scheduleOperation(
        std::move(mapping),
        [slice = std::move(slice), iter = std::move(iter),
         weakManager = manager.weak_from_this(), writeOptions](
            StorageManager::IStorageEngineMethods& methods) mutable noexcept {
          auto lastIndex = slice.getLastIndex();
          auto&& fut = methods.insert(std::move(iter), writeOptions);

          if (writeOptions.waitForSync) {
            return std::move(fut).thenValue(
                [lastIndex, weakManager = std::move(weakManager)](auto&& res) {
                  if (auto mngr = weakManager.lock(); res.ok() && mngr) {
                    mngr->updateSyncIndex(lastIndex);
                  }
                  return res.result();
                });
          }

          return std::move(fut).thenValue([lastIndex,
                                           weakManager = std::move(weakManager),
                                           &methods](auto&& res) mutable {
            if (auto mngr = weakManager.lock(); res.ok() && mngr) {
              // Methods are available as long as the manager is not
              // destroyed.
              methods.waitForSync(res.get()).thenFinal(
                  [lastIndex,
                   weakManager = std::move(weakManager)](auto&& tryRes) {
                    auto res = basics::catchToResult([&] {
                      return std::forward<decltype(tryRes)>(tryRes).get();
                    });
                    if (auto mngr = weakManager.lock()) {
                      if (res.fail()) {
                        LOG_CTX("6e64c", TRACE, mngr->loggerContext)
                            << "Will not update syncIndex from "
                            << mngr->syncIndex.getLockedGuard().get() << " to "
                            << lastIndex << ": " << res;
                        return;
                      }
                      mngr->updateSyncIndex(lastIndex);
                    }
                  });
            }
            return res.result();
          });
        });
  }

  explicit StorageManagerTransaction(GuardType guard, StorageManager& manager)
      : guard(std::move(guard)), manager(manager) {}

  GuardType guard;
  StorageManager& manager;
};

StorageManager::StorageManager(std::unique_ptr<IStorageEngineMethods>&& methods,
                               LoggerContext const& loggerContext,
                               std::shared_ptr<IScheduler> scheduler)
    : guardedData(std::move(methods)),
      loggerContext(
          loggerContext.with<logContextKeyLogComponent>("storage-manager")),
      scheduler(std::move(scheduler)) {}

auto StorageManager::resign() noexcept
    -> std::unique_ptr<IStorageEngineMethods> {
  auto guard = guardedData.getLockedGuard();
  auto methods = std::move(guard->methods);  // queue will be resolved
  guard.unlock();
  methods->waitForCompletion();
  return methods;
}

StorageManager::GuardedData::GuardedData(
    std::unique_ptr<IStorageEngineMethods>&& methods_ptr)
    :  // initialize info before moving the methods_ptr, in case the info
       // constructor throws
      info(methods_ptr->readMetadata().get()),
      methods(std::move(methods_ptr)) {
  ADB_PROD_ASSERT(methods != nullptr);
  try {
    spearheadMapping = onDiskMapping = computeTermIndexMap();
  } catch (...) {
    // We must not lose the core on exceptions
    methods_ptr = std::move(methods);
  }
}

auto StorageManager::GuardedData::computeTermIndexMap() const
    -> TermIndexMapping {
  TermIndexMapping mapping;
  auto iter = methods->getIterator(
      storage::IteratorPosition::fromLogIndex(LogIndex{0}));
  while (auto entry = iter->next()) {
    auto& e = *entry;
    mapping.insert(e.position(), e.entry().logTerm());
  }
  return mapping;
}

auto StorageManager::scheduleOperation(
    GuardType&& guard, TermIndexMapping mapResult,
    std::unique_ptr<StorageOperation> operation)
    -> futures::Future<arangodb::Result> {
  guard->spearheadMapping = mapResult;
  auto f = guard->queue.emplace_back(std::move(operation), std::move(mapResult))
               .promise.getFuture();
  triggerQueueWorker(std::move(guard));
  return f;
}

template<typename F>
auto StorageManager::scheduleOperationLambda(GuardType&& guard,
                                             TermIndexMapping mapResult, F&& fn)
    -> futures::Future<Result> {
  struct LambdaOperation : F, StorageOperation {
    explicit LambdaOperation(F&& fn) : F(std::move(fn)) {}
    static_assert(std::is_nothrow_invocable_r_v<futures::Future<Result>, F,
                                                IStorageEngineMethods&>);
    futures::Future<Result> run(
        IStorageEngineMethods& methods) noexcept override {
      return F::operator()(methods);
    }
  };

  return scheduleOperation(
      std::move(guard), std::move(mapResult),
      std::make_unique<LambdaOperation>(std::forward<F>(fn)));
}

void StorageManager::triggerQueueWorker(GuardType guard) noexcept {
  auto const worker = [](GuardType guard,
                         std::shared_ptr<StorageManager> self) noexcept
      -> futures::Future<futures::Unit> {
    auto const resolvePromise = [&](futures::Promise<Result> p, Result res) {
      self->scheduler->queue(
          [p = std::move(p), res]() mutable { p.setValue(std::move(res)); });
    };

    LOG_CTX("6efe9", TRACE, self->loggerContext)
        << "starting new storage worker";
    while (true) {
      if (guard->queue.empty()) {
        LOG_CTX("882a0", TRACE, self->loggerContext)
            << "stopping storage worker";
        guard->workerActive = false;
        break;
      }

      auto req = std::move(guard->queue.front());
      guard->queue.pop_front();
      if (not guard->methods) {
        guard.unlock();
        LOG_CTX("4f5e3", DEBUG, self->loggerContext)
            << "aborting storage operation "
            << " because log core gone";
        resolvePromise(
            std::move(req.promise),
            Result{TRI_ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE,
                   "Storage operation aborted"});
        guard = self->guardedData.getLockedGuard();
        continue;
      }
      LOG_CTX("e0a6d", TRACE, self->loggerContext)
          << "executing storage operation";
      auto f = req.operation->run(*guard->methods);
      guard.unlock();
      Result result = co_await asResult(std::move(f));
      // lock guard to make effects visible
      if (result.ok()) {
        LOG_CTX("b6cbf", TRACE, self->loggerContext)
            << "storage operation completed";
        // first update the on disk state
        guard = self->guardedData.getLockedGuard();
        guard->onDiskMapping = std::move(req.mappingResult);
        guard.unlock();
        // then resolve the promise
        resolvePromise(std::move(req.promise), std::move(result));
        // now lock again
        guard = self->guardedData.getLockedGuard();
      } else {
        LOG_CTX("77587", ERR, self->loggerContext)
            << "failed to commit storage operation: " << result;
        // lock directly, and flush all the queue.
        // If we resolve directly (which could result in a retry) we would
        // immediately abort any retries with `precondition` failed
        guard = self->guardedData.getLockedGuard();
        // restore old state
        guard->spearheadMapping = guard->onDiskMapping;
        // clear queue
        auto queue = std::move(guard->queue);
        guard->queue.clear();
        guard.unlock();
        // resolve everything else
        resolvePromise(std::move(req.promise), std::move(result));
        for (auto& r : queue) {
          LOG_CTX("507fe", INFO, self->loggerContext)
              << "aborting storage operation because of error in previous "
                 "operation";
          resolvePromise(std::move(r.promise),
                         TRI_ERROR_REPLICATION_REPLICATED_LOG_SUBSEQUENT_FAULT);
        }
        // and lock again
        guard = self->guardedData.getLockedGuard();
      }
    }

    co_return;
  };

  // check if a thread is working on the queue
  if (not guard->workerActive) {
    // otherwise start a worker
    guard->workerActive = true;
    std::ignore = worker(std::move(guard), shared_from_this());
  }
}

auto StorageManager::transaction() -> std::unique_ptr<IStorageTransaction> {
  auto guard = guardedData.getLockedGuard();
  if (guard->methods == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE,
        "Participant gone while trying to start storage manager transaction");
  }
  LOG_CTX("63ab8", TRACE, loggerContext) << "begin log transaction ";
  return std::make_unique<StorageManagerTransaction>(std::move(guard), *this);
}

struct comp::StateInfoTransaction : IStateInfoTransaction {
  using GuardType = Guarded<StorageManager::GuardedData>::mutex_guard_type;

  explicit StateInfoTransaction(GuardType guard, StorageManager& manager)
      : info(guard->info), guard(std::move(guard)), manager(manager) {}

  auto get() noexcept -> InfoType& override { return info; }

  storage::PersistedStateInfo info;
  GuardType guard;
  StorageManager& manager;
};

auto StorageManager::beginMetaInfoTrx()
    -> std::unique_ptr<IStateInfoTransaction> {
  auto guard = guardedData.getLockedGuard();
  if (guard->methods == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE,
        "Participant gone while trying to start meta info transaction");
  }
  LOG_CTX("ceb65", TRACE, loggerContext) << "begin meta info transaction";
  return std::make_unique<StateInfoTransaction>(std::move(guard), *this);
}

arangodb::Result StorageManager::commitMetaInfoTrx(
    std::unique_ptr<IStateInfoTransaction> ptr) {
  auto& trx = dynamic_cast<StateInfoTransaction&>(*ptr);
  auto guard = std::move(trx.guard);
  if (auto res = guard->methods->updateMetadata(trx.info); res.fail()) {
    LOG_CTX("0cb60", ERR, loggerContext)
        << "failed to commit meta data: " << res;
    THROW_ARANGO_EXCEPTION(res);
  }
  LOG_CTX("6a7fb", DEBUG, loggerContext)
      << "committed meta info transaction, new value = "
      << velocypack::serialize(trx.info).toJson();
  guard->info = std::move(trx.info);
  return {};
}

auto StorageManager::getCommittedMetaInfo() const
    -> storage::PersistedStateInfo {
  return guardedData.getLockedGuard()->info;
}

auto StorageManager::getTermIndexMapping() const -> TermIndexMapping {
  return guardedData.getLockedGuard()->onDiskMapping;
}

auto StorageManager::getLogIterator(LogIndex first) const
    -> std::unique_ptr<LogIterator> {
  return getLogIterator(
      LogRange{first, LogIndex{static_cast<std::uint64_t>(-1)}});
}

auto StorageManager::getLogIterator(std::optional<LogRange> bounds) const
    -> std::unique_ptr<LogIterator> {
  auto range = bounds.value_or(
      LogRange{LogIndex{0}, LogIndex{static_cast<std::uint64_t>(-1)}});

  auto guard = guardedData.getLockedGuard();
  if (guard->methods == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE,
        "Participant gone while trying to get a persisted log iterator");
  }

  auto diskIter = guard->methods->getIterator(
      storage::IteratorPosition::fromLogIndex(range.from));

  struct Iterator : LogIterator {
    explicit Iterator(LogRange range,
                      std::unique_ptr<PersistedLogIterator> disk)
        : _range(range), _disk(std::move(disk)) {}

    auto next() -> std::optional<LogEntry> override {
      auto entry = _disk->next();
      if (not entry) {
        return std::nullopt;
      }
      if (not _range.contains(entry->entry().logIndex())) {
        return std::nullopt;  // end of range
      }
      return entry->entry();
    }

    LogRange _range;
    std::unique_ptr<PersistedLogIterator> _disk;
  };

  return std::make_unique<Iterator>(range, std::move(diskIter));
}

auto StorageManager::getCommittedLogIterator(std::optional<LogRange> bounds)
    const -> std::unique_ptr<LogViewRangeIterator> {
  auto guard = guardedData.getLockedGuard();
  if (guard->methods == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_PARTICIPANT_GONE,
        "Participant gone while trying to get committed log "
        "iterator");
  }

  auto range = guard->onDiskMapping.getIndexRange();
  if (bounds) {
    range = intersect(*bounds, range);
  }
  auto diskIter = guard->methods->getIterator(
      storage::IteratorPosition::fromLogIndex(range.from));

  struct Iterator : LogViewRangeIterator {
    explicit Iterator(LogRange range,
                      std::unique_ptr<PersistedLogIterator> disk)
        : _range(range), _disk(std::move(disk)) {}

    auto range() const noexcept -> LogRange override { return _range; }
    auto next() -> std::optional<LogEntryView> override {
      while (true) {
        _entry = _disk->next();
        if (not _entry) {
          return std::nullopt;
        }
        if (not _range.contains(_entry->entry().logIndex())) {
          return std::nullopt;  // end of range
        }
        if (_entry->entry().hasPayload()) {
          return LogEntryView{_entry->entry().logIndex(),
                              *_entry->entry().logPayload()};
        }
      }
    }

    LogRange _range;
    std::unique_ptr<PersistedLogIterator> _disk;
    std::optional<PersistedLogEntry> _entry;
  };

  return std::make_unique<Iterator>(range, std::move(diskIter));
}

auto StorageManager::getSyncIndex() const -> LogIndex {
  return syncIndex.getLockedGuard().get();
}

void StorageManager::updateSyncIndex(LogIndex index) {
  syncIndex.doUnderLock([index](auto& currentIndex) {
    currentIndex = std::max(currentIndex, index);
  });
}

StorageManager::StorageRequest::StorageRequest(
    std::unique_ptr<StorageOperation> op, TermIndexMapping mappingResult)
    : operation(std::move(op)), mappingResult(std::move(mappingResult)) {}
StorageManager::StorageRequest::~StorageRequest() = default;

}  // namespace arangodb::replication2::replicated_log
