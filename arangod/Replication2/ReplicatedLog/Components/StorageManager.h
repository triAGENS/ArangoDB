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
#pragma once
#include "Replication2/ReplicatedLog/Components/IStorageManager.h"
#include "Replication2/Storage/PersistedStateInfo.h"
#include "Basics/Guarded.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/TermIndexMapping.h"
#include "Futures/Promise.h"
#include <deque>
namespace arangodb::replication2 {
struct IScheduler;
}
namespace arangodb::replication2::replicated_log {
inline namespace comp {

struct StorageManagerTransaction;
struct StateInfoTransaction;
struct StorageManager : IStorageManager,
                        std::enable_shared_from_this<StorageManager> {
  using IStorageEngineMethods = storage::IStorageEngineMethods;

  StorageManager(std::unique_ptr<IStorageEngineMethods>&& core,
                 LoggerContext const& loggerContext,
                 std::shared_ptr<IScheduler> scheduler);
  auto resign() noexcept -> std::unique_ptr<IStorageEngineMethods>;
  auto transaction() -> std::unique_ptr<IStorageTransaction> override;
  auto getCommittedLogIterator(std::optional<LogRange> range) const
      -> std::unique_ptr<LogViewRangeIterator> override;
  auto getLogIterator(LogIndex first) const
      -> std::unique_ptr<LogIterator> override;
  auto getLogIterator(std::optional<LogRange> range) const
      -> std::unique_ptr<LogIterator>;
  auto getTermIndexMapping() const -> TermIndexMapping override;
  auto beginMetaInfoTrx() -> std::unique_ptr<IStateInfoTransaction> override;
  auto commitMetaInfoTrx(std::unique_ptr<IStateInfoTransaction> ptr)
      -> Result override;
  auto getCommittedMetaInfo() const -> storage::PersistedStateInfo override;
  auto getSyncIndex() const -> LogIndex override;

 private:
  friend struct StorageManagerTransaction;
  friend struct StateInfoTransaction;

  struct GuardedData;

  struct StorageOperation;

  struct StorageRequest {
    std::unique_ptr<StorageOperation> operation;
    TermIndexMapping mappingResult;
    futures::Promise<Result> promise;

    ~StorageRequest();
    StorageRequest(StorageRequest&&) noexcept = default;
    explicit StorageRequest(std::unique_ptr<StorageOperation> op,
                            TermIndexMapping mappingResult);
  };

  struct GuardedData {
    explicit GuardedData(std::unique_ptr<IStorageEngineMethods>&& methods);

    TermIndexMapping onDiskMapping, spearheadMapping;
    storage::PersistedStateInfo info;
    std::unique_ptr<IStorageEngineMethods> methods;
    std::deque<StorageRequest> queue;
    bool workerActive{false};

    auto computeTermIndexMap() const -> TermIndexMapping;
  };
  Guarded<GuardedData> guardedData;
  using GuardType = Guarded<GuardedData>::mutex_guard_type;
  LoggerContext const loggerContext;
  std::shared_ptr<IScheduler> const scheduler;

  // The syncIndex is initially 0 and will be updated to its real value when the
  // first append-entries is synced.
  Guarded<LogIndex> syncIndex;

  auto scheduleOperation(GuardType&&, TermIndexMapping mapResult,
                         std::unique_ptr<StorageOperation>)
      -> futures::Future<Result>;
  template<typename F>
  auto scheduleOperationLambda(GuardType&&, TermIndexMapping mapResult, F&&)
      -> futures::Future<Result>;
  void triggerQueueWorker(GuardType) noexcept;
  void updateSyncIndex(LogIndex index);
};

}  // namespace comp
}  // namespace arangodb::replication2::replicated_log
