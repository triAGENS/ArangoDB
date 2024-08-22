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

#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/NetworkMessages.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/ReplicatedLog/WaitForBag.h"
#include "Replication2/ReplicatedLog/types.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "TermInformation.h"

#include <Basics/Guarded.h>
#include <Futures/Future.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace arangodb::replication2::replicated_log {
inline namespace comp {
struct StorageManager;
struct CompactionManager;
struct StateHandleManager;
struct SnapshotManager;
struct FollowerCommitManager;
struct AppendEntriesManager;
struct MethodsProviderManager;
struct MessageIdManager;
}  // namespace comp
}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::replicated_log {

struct FollowerManager {
  explicit FollowerManager(
      std::unique_ptr<storage::IStorageEngineMethods>&& methods,
      std::unique_ptr<IReplicatedStateHandle> stateHandlePtr,
      std::shared_ptr<FollowerTermInformation const> termInfo,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options,
      std::shared_ptr<ReplicatedLogMetrics> metrics,
      std::shared_ptr<ILeaderCommunicator>,
      std::shared_ptr<IScheduler> scheduler, LoggerContext logContext);
  ~FollowerManager();
  auto getStatus() const -> LogStatus;
  auto getQuickStatus() const -> QuickLogStatus;
  auto resign()
      -> std::tuple<std::unique_ptr<storage::IStorageEngineMethods>,
                    std::unique_ptr<IReplicatedStateHandle>, DeferredAction>;

  auto appendEntries(AppendEntriesRequest request)
      -> futures::Future<AppendEntriesResult>;

 private:
  friend struct LogFollowerImpl;
  LoggerContext const loggerContext;
  std::shared_ptr<ReplicatedLogGlobalSettings const> const options;
  std::shared_ptr<ReplicatedLogMetrics> const metrics;

  // Make this into shared_ptrs allows types to remain incomplete in this header
  std::shared_ptr<StorageManager> const storage;
  std::shared_ptr<CompactionManager> const compaction;
  std::shared_ptr<FollowerCommitManager> const commit;
  std::shared_ptr<StateHandleManager> const stateHandle;
  std::shared_ptr<SnapshotManager> const snapshot;
  std::shared_ptr<MessageIdManager> const messageIdManager;
  std::shared_ptr<MethodsProviderManager> const methodsProvider;
  std::shared_ptr<AppendEntriesManager> const appendEntriesManager;
  std::shared_ptr<FollowerTermInformation const> const termInfo;
};

struct LogFollowerImpl : ILogFollower {
  explicit LogFollowerImpl(
      ParticipantId myself,
      std::unique_ptr<storage::IStorageEngineMethods>&& methods,
      std::unique_ptr<IReplicatedStateHandle> stateHandlePtr,
      std::shared_ptr<FollowerTermInformation const> termInfo,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options,
      std::shared_ptr<ReplicatedLogMetrics> metrics,
      std::shared_ptr<ILeaderCommunicator> leaderComm,
      std::shared_ptr<IScheduler> scheduler, LoggerContext logContext);

  auto getStatus() const -> LogStatus override;

  auto getQuickStatus() const -> QuickLogStatus override;

  [[nodiscard]] auto resign() && -> std::tuple<
      std::unique_ptr<storage::IStorageEngineMethods>,
      std::unique_ptr<IReplicatedStateHandle>, DeferredAction> override;

  auto waitFor(LogIndex index) -> WaitForFuture override;
  auto waitForIterator(LogIndex index) -> WaitForIteratorFuture override;

  [[nodiscard]] auto getInternalLogIterator(std::optional<LogRange> bounds)
      const -> std::unique_ptr<LogIterator> override;

  auto compact() -> ResultT<CompactionResult> override;

  auto getParticipantId() const noexcept -> ParticipantId const& override;

  auto appendEntries(AppendEntriesRequest request)
      -> futures::Future<AppendEntriesResult> override;

  ParticipantId const myself;
  Guarded<FollowerManager> guarded;
};

}  // namespace arangodb::replication2::replicated_log
