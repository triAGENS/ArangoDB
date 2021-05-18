////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#ifndef ARANGODB3_ROCKSDBLOG_H
#define ARANGODB3_ROCKSDBLOG_H

#include <rocksdb/db.h>

#include "Replication2/ReplicatedLog/PersistedLog.h"
#include "RocksDBKeyBounds.h"

namespace arangodb {


struct RocksDBLogPersistor : std::enable_shared_from_this<RocksDBLogPersistor> {
  struct Executor {
    virtual ~Executor() = default;
    virtual void operator()(fu2::unique_function<void()>) = 0;
  };

  RocksDBLogPersistor(rocksdb::ColumnFamilyHandle* cf, rocksdb::DB* db,
                      std::shared_ptr<Executor> executor);

  struct WriteOptions {
    bool waitForSync = false;
  };

  auto persist(std::shared_ptr<arangodb::replication2::PersistedLog> log,
               std::unique_ptr<arangodb::replication2::LogIterator> iter,
               WriteOptions const& options)
  -> futures::Future<Result>;

  //std::mutex _persistorMutex;
  std::atomic<unsigned> _activePersistorThreads = 0;
  struct PersistRequest {
    PersistRequest(std::shared_ptr<arangodb::replication2::PersistedLog> log,
                   std::unique_ptr<arangodb::replication2::LogIterator> iter,
                   futures::Promise<Result> promise)
        : log(std::move(log)), iter(std::move(iter)), promise(std::move(promise)) {}
    std::shared_ptr<arangodb::replication2::PersistedLog> log;
    std::unique_ptr<arangodb::replication2::LogIterator> iter;
    futures::Promise<Result> promise;
  };

  //std::vector<PersistRequest> _pendingPersistRequests;


  struct Lane {
    Lane() = default;
    std::mutex _persistorMutex;
    std::vector<PersistRequest> _pendingPersistRequests;
    bool _waitForSync = false;
  };


  void runPersistorWorker(Lane& lane) noexcept;

  Lane _lanes[2] = {};

  rocksdb::ColumnFamilyHandle* const _cf;
  rocksdb::DB* const _db;
  std::shared_ptr<Executor> _executor;
};

class RocksDBLog : public replication2::PersistedLog, public std::enable_shared_from_this<RocksDBLog> {
 public:
  ~RocksDBLog() override = default;
  RocksDBLog(replication2::LogId id, uint64_t objectId, std::shared_ptr<RocksDBLogPersistor> persistor);

  auto insert(replication2::LogIterator& iter, WriteOptions const&) -> Result override;
  auto insertAsync(std::unique_ptr<replication2::LogIterator> iter,
                   WriteOptions const&) -> futures::Future<Result> override;
  auto insertWithBatch(replication2::LogIterator& iter, rocksdb::WriteBatch& batch) -> Result;
  auto insertSingleWrites(replication2::LogIterator& iter) -> Result;
  auto read(replication2::LogIndex start)
      -> std::unique_ptr<replication2::LogIterator> override;
  auto removeFront(replication2::LogIndex stop) -> Result override;
  auto removeBack(replication2::LogIndex start) -> Result override;


  uint64_t objectId() const { return _objectId; }

  auto drop() -> Result override;


  RocksDBKeyBounds getBounds() const { return RocksDBKeyBounds::LogRange(_objectId); }

 protected:
  uint64_t const _objectId;
  std::shared_ptr<RocksDBLogPersistor> _persistor;
};

}  // namespace arangodb

#endif  // ARANGODB3_ROCKSDBLOG_H
