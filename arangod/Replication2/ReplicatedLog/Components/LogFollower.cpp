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

#include "LogFollower.h"
#include "Logger/LogContextKeys.h"

#include "Metrics/Gauge.h"

#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/ReplicatedLog/Components/SnapshotManager.h"
#include "Replication2/ReplicatedLog/Components/StorageManager.h"
#include "Replication2/ReplicatedLog/Components/CompactionManager.h"
#include "Replication2/ReplicatedLog/Components/FollowerCommitManager.h"
#include "Replication2/ReplicatedLog/Components/AppendEntriesManager.h"
#include "Replication2/ReplicatedLog/Components/StateHandleManager.h"
#include "Replication2/ReplicatedLog/Components/MethodsProvider.h"
#include "Replication2/ReplicatedLog/Components/MessageIdManager.h"
#include "Replication2/Storage/IStorageEngineMethods.h"

using namespace arangodb;
using namespace arangodb::replication2;

namespace arangodb::replication2::replicated_log {

namespace {

auto deriveLoggerContext(FollowerTermInformation const& info,
                         LoggerContext inCtx = LoggerContext{
                             Logger::REPLICATION2}) -> LoggerContext {
  return inCtx.with<logContextKeyStateRole>("follower")
      .with<logContextKeyTerm>(info.term)
      .with<logContextKeyLeaderId>(info.leader.value_or("<none>"));
}

}  // namespace

FollowerManager::FollowerManager(
    std::unique_ptr<storage::IStorageEngineMethods>&& storageMethods,
    std::unique_ptr<IReplicatedStateHandle> stateHandlePtr,
    std::shared_ptr<FollowerTermInformation const> termInfo,
    std::shared_ptr<ReplicatedLogGlobalSettings const> options,
    std::shared_ptr<ReplicatedLogMetrics> metrics,
    std::shared_ptr<ILeaderCommunicator> leaderComm,
    std::shared_ptr<IScheduler> scheduler, LoggerContext logContext)
    : loggerContext(deriveLoggerContext(*termInfo, logContext)),
      options(options),
      metrics(metrics),
      storage(std::make_shared<StorageManager>(std::move(storageMethods),
                                               loggerContext, scheduler)),
      compaction(std::make_shared<CompactionManager>(*storage, options,
                                                     loggerContext)),
      commit(std::make_shared<FollowerCommitManager>(*storage, loggerContext,
                                                     scheduler)),
      stateHandle(std::make_shared<StateHandleManager>(
          std::move(stateHandlePtr), *commit)),
      snapshot(std::make_shared<SnapshotManager>(
          *storage, *stateHandle, termInfo, leaderComm, loggerContext)),
      messageIdManager(std::make_shared<MessageIdManager>()),
      methodsProvider(std::make_shared<MethodsProviderManager>(
          commit, storage, compaction, snapshot, messageIdManager)),
      appendEntriesManager(std::make_shared<AppendEntriesManager>(
          termInfo, *storage, *snapshot, *compaction, *stateHandle,
          *messageIdManager, metrics, loggerContext)),
      termInfo(termInfo) {
  metrics->replicatedLogFollowerNumber->operator++();
  // TODO The following line creates a dependency loop: It means the StateHandle
  //      depends on the MethodsProvider, which isn't currently explicit in the
  //      constructors. This creates the last edge in the loop
  //               MethodsProviderManager
  //        ->     SnapshotManager
  //        ->     StateHandleManager
  //        -(!)-> MethodsProviderManager
  //      which can (and probably does) lead to lock
  //      inversions. We should break it up.
  try {
    stateHandle->becomeFollower(methodsProvider->getMethods());
    // Follower state manager is there, now get a snapshot if we need one.
    snapshot->acquireSnapshotIfNecessary();
  } catch (...) {
    // We must not lose the core on exceptions
    storageMethods = storage->resign();
    throw;
  }
}

FollowerManager::~FollowerManager() {
  metrics->replicatedLogFollowerNumber->operator--();
}

auto FollowerManager::getStatus() const -> LogStatus {
  auto commitIndex = commit->getCommitIndex();
  auto mapping = storage->getTermIndexMapping();
  auto syncIndex = storage->getSyncIndex();
  auto [releaseIndex, lowestIndexToKeep] = compaction->getIndexes();
  auto const stateStatus = stateHandle->getInternalStatus();
  auto const appliedIndex = std::visit(
      overload{
          [&](replicated_state::Status::Follower const& status) {
            using namespace replicated_state;
            return std::visit(overload{
                                  [&](Status::Follower::Constructed const& c) {
                                    return c.appliedIndex;
                                  },
                                  [](auto const&) { return LogIndex{0}; },
                              },
                              status.value);
          },
          [](auto const&) { return LogIndex{0}; },

      },
      stateStatus.value);
  return LogStatus{FollowerStatus{
      .local =
          LogStatistics{
              .spearHead = mapping.getLastIndex().value_or(TermIndexPair{}),
              .commitIndex = commitIndex,
              .firstIndex =
                  mapping.getFirstIndex().value_or(TermIndexPair{}).index,
              .releaseIndex = releaseIndex,
              .syncIndex = syncIndex,
              .lowestIndexToKeep = lowestIndexToKeep,
              .appliedIndex = appliedIndex,
          },
      .leader = termInfo->leader,
      .term = termInfo->term,
      .lowestIndexToKeep = lowestIndexToKeep,
      .compactionStatus = compaction->getCompactionStatus(),
      .snapshotAvailable =
          snapshot->checkSnapshotState() == SnapshotState::AVAILABLE,
  }};
}
namespace {
auto getLocalState(LogIndex const& commitIndex, bool const snapshotAvailable,
                   replicated_state::Status const& stateStatus)
    -> LocalStateMachineStatus {
  return std::visit(
      overload{
          [&](replicated_state::Status::Follower const& status) {
            using namespace replicated_state;
            return std::visit(
                overload{
                    [](Status::Follower::Resigned const&) {
                      return LocalStateMachineStatus::kUnconfigured;
                    },
                    [&](Status::Follower::Constructed const&) {
                      bool const leaderConnectionEstablished =
                          commitIndex > LogIndex{0};
                      if (!leaderConnectionEstablished) {
                        return LocalStateMachineStatus::kConnecting;
                      } else if (!snapshotAvailable) {
                        return LocalStateMachineStatus::kAcquiringSnapshot;
                      } else {
                        return LocalStateMachineStatus::kOperational;
                      }
                    },
                },
                status.value);
          },
          [](replicated_state::Status::Unconfigured const&) {
            return LocalStateMachineStatus::kUnconfigured;
          },
          [](replicated_state::Status::Leader const&) {
            return LocalStateMachineStatus::kUnconfigured;
          },
      },
      stateStatus.value);
}
}  // namespace

auto FollowerManager::getQuickStatus() const -> QuickLogStatus {
  // Please note that it is important that the commit index is checked before
  // the snapshot. Otherwise the local state could be reported operational,
  // while it isn't (and never was during this term).
  // This is because the snapshot status can toggle once from available to
  // missing (if it started as available), before eventually toggling from
  // missing to available. The commit index starts as zero and can only
  // increase. The toggle *to* missing will happen before any change to the
  // commit index.
  // The local state is operational if a) the commit index is greater
  // than zero, and b) the snapshot is available. That means checking them in
  // the wrong order could see the snapshot status available from before it was
  // toggled to missing, and then the commit index that was just increased.
  auto const commitIndex = commit->getCommitIndex();
  auto const mapping = storage->getTermIndexMapping();
  auto const syncIndex = storage->getSyncIndex();
  auto const [releaseIndex, lowestIndexToKeep] = compaction->getIndexes();
  bool const snapshotAvailable =
      snapshot->checkSnapshotState() == SnapshotState::AVAILABLE;
  auto const stateStatus = stateHandle->getInternalStatus();

  auto const localState =
      getLocalState(commitIndex, snapshotAvailable, stateStatus);
  auto const appliedIndex = std::visit(
      overload{
          [&](replicated_state::Status::Follower const& status) {
            using namespace replicated_state;
            return std::visit(overload{
                                  [&](Status::Follower::Constructed const& c) {
                                    return c.appliedIndex;
                                  },
                                  [](auto const&) { return LogIndex{0}; },
                              },
                              status.value);
          },
          [](auto const&) { return LogIndex{0}; },

      },
      stateStatus.value);

  return QuickLogStatus{
      .role = ParticipantRole::kFollower,
      .localState = localState,
      .term = termInfo->term,
      .local =
          LogStatistics{
              .spearHead = mapping.getLastIndex().value_or(TermIndexPair{}),
              .commitIndex = commitIndex,
              .firstIndex =
                  mapping.getFirstIndex().value_or(TermIndexPair{}).index,
              .releaseIndex = releaseIndex,
              .syncIndex = syncIndex,
              .lowestIndexToKeep = lowestIndexToKeep,
              .appliedIndex = appliedIndex,
          },
      .leadershipEstablished = commitIndex.value > 0,
      .snapshotAvailable = snapshotAvailable,
  };
}

auto FollowerManager::resign()
    -> std::tuple<std::unique_ptr<storage::IStorageEngineMethods>,
                  std::unique_ptr<IReplicatedStateHandle>, DeferredAction> {
  // 1. resign the state and receive its handle
  auto handle = stateHandle->resign();
  // 2. resign the storage manager to receive the storage engine methods
  auto methods = storage->resign();
  // 3. resign append entries manager, so append entries requests in flight
  // don't try to access other managers after this
  std::move((*appendEntriesManager)).resign();
  // 4. abort all wait for promises.
  commit->resign();
  return std::make_tuple(std::move(methods), std::move(handle),
                         DeferredAction{});
}

auto FollowerManager::appendEntries(AppendEntriesRequest request)
    -> futures::Future<AppendEntriesResult> {
  return appendEntriesManager->appendEntries(std::move(request));
}

auto LogFollowerImpl::getQuickStatus() const -> QuickLogStatus {
  return guarded.getLockedGuard()->getQuickStatus();
}

auto LogFollowerImpl::getStatus() const -> LogStatus {
  return guarded.getLockedGuard()->getStatus();
}

auto LogFollowerImpl::resign() && -> std::tuple<
    std::unique_ptr<storage::IStorageEngineMethods>,
    std::unique_ptr<IReplicatedStateHandle>, DeferredAction> {
  return guarded.getLockedGuard()->resign();
}

auto LogFollowerImpl::waitFor(LogIndex index)
    -> ILogParticipant::WaitForFuture {
  return guarded.getLockedGuard()->commit->waitFor(index);
}

auto LogFollowerImpl::waitForIterator(LogIndex index)
    -> ILogParticipant::WaitForIteratorFuture {
  return guarded.getLockedGuard()->commit->waitForIterator(index);
}

auto LogFollowerImpl::getInternalLogIterator(
    std::optional<LogRange> bounds) const -> std::unique_ptr<LogIterator> {
  return guarded.getLockedGuard()->storage->getLogIterator(bounds);
}

auto LogFollowerImpl::compact() -> ResultT<CompactionResult> {
  // TODO clean up CompacionResult vs ICompactionManager::CompactResult
  auto result = guarded.getLockedGuard()->compaction->compact().waitAndGet();
  if (result.error) {
    return Result{result.error->errorNumber(), result.error->errorMessage()};
  }
  return CompactionResult{.numEntriesCompacted = result.compactedRange.count(),
                          .range = result.compactedRange,
                          .stopReason = result.stopReason};
}

auto LogFollowerImpl::getParticipantId() const noexcept
    -> ParticipantId const& {
  return myself;
}

auto LogFollowerImpl::appendEntries(AppendEntriesRequest request)
    -> futures::Future<AppendEntriesResult> {
  return guarded.getLockedGuard()->appendEntriesManager->appendEntries(
      std::move(request));
}

LogFollowerImpl::LogFollowerImpl(
    ParticipantId myself,
    std::unique_ptr<storage::IStorageEngineMethods>&& methods,
    std::unique_ptr<IReplicatedStateHandle> stateHandlePtr,
    std::shared_ptr<const FollowerTermInformation> termInfo,
    std::shared_ptr<const ReplicatedLogGlobalSettings> options,
    std::shared_ptr<ReplicatedLogMetrics> metrics,
    std::shared_ptr<ILeaderCommunicator> leaderComm,
    std::shared_ptr<IScheduler> scheduler, LoggerContext logContext)
    : myself(std::move(myself)),
      guarded(std::move(methods), std::move(stateHandlePtr),
              std::move(termInfo), std::move(options), std::move(metrics),
              std::move(leaderComm), std::move(scheduler), logContext) {}

}  // namespace arangodb::replication2::replicated_log
