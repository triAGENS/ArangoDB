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

#pragma once

#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/LogCore.h"
#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/PersistedLog.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"

namespace arangodb::replication2::test {

using namespace replicated_log;

struct MockLog : replication2::replicated_log::PersistedLog {
  using storeType = std::map<replication2::LogIndex, replication2::LogEntry>;

  explicit MockLog(replication2::LogId id);
  explicit MockLog(replication2::GlobalLogIdentifier gid);
  MockLog(replication2::LogId id, storeType storage);

  auto insert(replication2::LogIterator& iter, WriteOptions const&)
      -> Result override;
  auto insertAsync(std::unique_ptr<replication2::LogIterator> iter,
                   WriteOptions const&) -> futures::Future<Result> override;
  auto read(replication2::LogIndex start)
      -> std::unique_ptr<replication2::LogIterator> override;
  auto removeFront(replication2::LogIndex stop)
      -> futures::Future<Result> override;
  auto removeBack(replication2::LogIndex start) -> Result override;
  auto drop() -> Result override;

  void setEntry(replication2::LogIndex idx, replication2::LogTerm term,
                replication2::LogPayload payload);
  void setEntry(replication2::LogEntry);

  [[nodiscard]] storeType getStorage() const { return _storage; }

  auto checkEntryWaitedForSync(LogIndex idx) const noexcept -> bool {
    return _writtenWithWaitForSync.find(idx) != _writtenWithWaitForSync.end();
  }

 private:
  using iteratorType = storeType::iterator;
  storeType _storage;
  std::unordered_set<LogIndex> _writtenWithWaitForSync;
};

struct DelayedMockLog : MockLog {
  explicit DelayedMockLog(replication2::LogId id) : MockLog(id) {}

  auto insertAsync(std::unique_ptr<replication2::LogIterator> iter,
                   WriteOptions const&) -> futures::Future<Result> override;

  auto hasPendingInsert() const noexcept -> bool {
    return _pending.has_value();
  }
  void runAsyncInsert();

  struct PendingRequest {
    PendingRequest(std::unique_ptr<replication2::LogIterator> iter,
                   WriteOptions options);
    std::unique_ptr<replication2::LogIterator> iter;
    WriteOptions options;
    futures::Promise<Result> promise;
  };

  std::optional<PendingRequest> _pending;
};

struct AsyncMockLog : MockLog {
  explicit AsyncMockLog(replication2::LogId id);

  ~AsyncMockLog() noexcept;

  auto insertAsync(std::unique_ptr<replication2::LogIterator> iter,
                   WriteOptions const&) -> futures::Future<Result> override;

  auto stop() noexcept -> void {
    if (!_stopping) {
      {
        std::unique_lock guard(_mutex);
        _stopping = true;
        _cv.notify_all();
      }
      _asyncWorker.join();
    }
  }

 private:
  struct QueueEntry {
    WriteOptions opts;
    std::unique_ptr<replication2::LogIterator> iter;
    futures::Promise<Result> promise;
  };

  void runWorker();

  std::mutex _mutex;
  std::vector<std::shared_ptr<QueueEntry>> _queue;
  std::condition_variable _cv;
  std::atomic<bool> _stopping = false;
  bool _stopped = false;
  // _asyncWorker *must* be initialized last, otherwise starting the thread
  // races with initializing the coordination variables.
  std::thread _asyncWorker;
};

}  // namespace arangodb::replication2::test
