////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <Basics/Guarded.h>
#include <Containers/ImmerMemoryPolicy.h>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "Basics/Result.h"
#include "Cluster/CallbackGuard.h"
#include "Futures/Future.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/IRebootIdCache.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/NetworkMessages.h"
#include "Replication2/ReplicatedLog/WaitForBag.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/Storage/IteratorPosition.h"
#include "Scheduler/Scheduler.h"
#include "Replication2/IScheduler.h"
#include "Replication2/ReplicatedLog/Components/InMemoryLogManager.h"
#include "Replication2/ReplicatedLog/Components/StorageManager.h"
#include "Replication2/ReplicatedLog/Components/CompactionManager.h"

namespace arangodb {
struct DeferredAction;
}  // namespace arangodb

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of
// data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift
// intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/flex_vector.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

namespace arangodb::futures {
template<typename T>
class Try;
}
namespace arangodb::replication2::algorithms {
struct ParticipantState;
}
namespace arangodb::replication2::replicated_log {
struct ReplicatedLogMetrics;
}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::replicated_log {

/**
 * @brief Leader instance of a replicated log.
 */
class LogLeader : public std::enable_shared_from_this<LogLeader>,
                  public ILogLeader {
 public:
  ~LogLeader() override;

  [[nodiscard]] static auto construct(
      std::unique_ptr<storage::IStorageEngineMethods>&& methods,
      std::shared_ptr<agency::ParticipantsConfig const> participantsConfig,
      ParticipantId id, LogTerm term, LoggerContext const& logContext,
      std::shared_ptr<ReplicatedLogMetrics> logMetrics,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options,
      std::unique_ptr<IReplicatedStateHandle> stateHandle,
      std::shared_ptr<IAbstractFollowerFactory> followerFactory,
      std::shared_ptr<IScheduler> scheduler,
      std::shared_ptr<IRebootIdCache> rebootIdCache)
      -> std::shared_ptr<LogLeader>;

  [[nodiscard]] auto waitFor(LogIndex) -> WaitForFuture override;

  [[nodiscard]] auto waitForIterator(LogIndex index)
      -> WaitForIteratorFuture override;

  // Triggers sending of appendEntries requests to all followers. This continues
  // until all participants are perfectly in sync, and will then stop.
  // Is usually called automatically after an insert, but can be called manually
  // from test code.
  auto triggerAsyncReplication() -> void;

  [[nodiscard]] auto getStatus() const -> LogStatus override;

  [[nodiscard]] auto getQuickStatus() const -> QuickLogStatus override;

  auto resign() && -> std::tuple<
      std::unique_ptr<storage::IStorageEngineMethods>,
      std::unique_ptr<IReplicatedStateHandle>, DeferredAction> override;

  [[nodiscard]] auto getParticipantId() const noexcept -> ParticipantId const&;

  [[nodiscard]] auto compact() -> ResultT<CompactionResult> override;
  [[nodiscard]] auto getLogConsumerIterator(std::optional<LogRange> bounds)
      const -> std::unique_ptr<LogViewRangeIterator>;
  [[nodiscard]] auto getInternalLogIterator(std::optional<LogRange> bounds)
      const -> std::unique_ptr<LogIterator> override;

  auto waitForLeadership() -> WaitForFuture override;
  auto ping(std::optional<std::string> message) -> LogIndex override;

  // Updates the flags of the participants.
  auto updateParticipantsConfig(
      std::shared_ptr<agency::ParticipantsConfig const> const& config)
      -> LogIndex override;

  auto setSnapshotAvailable(ParticipantId const& participantId,
                            SnapshotAvailableReport report) -> Result;

 protected:
  // Use the named constructor construct() to create a leader!
  LogLeader(LoggerContext logContext,
            std::shared_ptr<ReplicatedLogMetrics> logMetrics,
            std::shared_ptr<ReplicatedLogGlobalSettings const> options,
            ParticipantId id, LogTerm term, LogIndex firstIndexOfCurrentTerm,
            std::unique_ptr<IReplicatedStateHandle>,
            std::shared_ptr<IAbstractFollowerFactory> followerFactory,
            std::shared_ptr<IScheduler> scheduler,
            std::shared_ptr<IRebootIdCache> rebootIdCache);

 private:
  struct GuardedLeaderData;

  using Guard = MutexGuard<GuardedLeaderData, std::unique_lock<std::mutex>>;
  using ConstGuard =
      MutexGuard<GuardedLeaderData const, std::unique_lock<std::mutex>>;

  struct alignas(64) FollowerInfo {
    explicit FollowerInfo(std::shared_ptr<AbstractFollower> impl,
                          LogIndex lastLogIndex,
                          LoggerContext const& logContext);

    std::chrono::steady_clock::duration _lastRequestLatency{};
    std::chrono::steady_clock::time_point _lastRequestStartTP{};
    std::chrono::steady_clock::time_point _errorBackoffEndTP{};
    std::shared_ptr<AbstractFollower> _impl;
    TermIndexPair lastAckedIndex = TermIndexPair{LogTerm{0}, LogIndex{0}};
    storage::IteratorPosition nextPrevLogPosition;
    LogIndex lastAckedCommitIndex = LogIndex{0};
    LogIndex lastAckedLowestIndexToKeep = LogIndex{0};
    LogIndex syncIndex = LogIndex{0};
    MessageId lastSentMessageId{0};
    std::size_t numErrorsSinceLastAnswer = 0;
    AppendEntriesErrorReason lastErrorReason;
    bool snapshotAvailable{true};
    MessageId snapshotAvailableMessageId;
    LoggerContext const logContext;
    IScheduler::WorkItemHandle lastRequestHandle;
    cluster::CallbackGuard rebootIdCallbackGuard;

    enum class State {
      IDLE,
      PREPARE,
      ERROR_BACKOFF,
      REQUEST_IN_FLIGHT,
    } _state = State::IDLE;
  };

  struct LocalFollower final : AbstractFollower {
    // The LocalFollower assumes that the last entry of log core matches
    // lastIndex.
    // The LogCore parameter will (and must) stay untouched in case of an
    // exception.
    LocalFollower(LogLeader& self, LoggerContext logContext,
                  std::shared_ptr<IStorageManager>, TermIndexPair lastIndex);
    ~LocalFollower() override = default;

    LocalFollower(LocalFollower const&) = delete;
    LocalFollower(LocalFollower&&) noexcept = delete;
    auto operator=(LocalFollower const&) -> LocalFollower& = delete;
    auto operator=(LocalFollower&&) noexcept -> LocalFollower& = delete;

    [[nodiscard]] auto getParticipantId() const noexcept
        -> ParticipantId const& override;
    [[nodiscard]] auto appendEntries(AppendEntriesRequest methods)
        -> arangodb::futures::Future<AppendEntriesResult> override;

    [[nodiscard]] auto release(LogIndex stop) const -> Result;

   private:
    LogLeader& _leader;
    LoggerContext const _logContext;
    std::shared_ptr<IStorageManager> const _storageManager;
  };

  struct PreparedAppendEntryRequest {
    PreparedAppendEntryRequest() = delete;
    PreparedAppendEntryRequest(
        std::shared_ptr<LogLeader> const& logLeader,
        std::shared_ptr<FollowerInfo> follower,
        std::chrono::steady_clock::duration executionDelay);

    std::weak_ptr<LogLeader> _parentLog;
    std::weak_ptr<FollowerInfo> _follower;
    std::chrono::steady_clock::duration _executionDelay;
  };

  struct ResolvedPromiseSet {
    LogIndex commitIndex;
    WaitForQueue _set;
    WaitForResult result;
  };

  struct InnerTermConfig {
    InnerTermConfig(agency::ParticipantsConfig participantsConfig,
                    std::unordered_map<ParticipantId, RebootId> safeRebootIds);

    agency::ParticipantsConfig const participantsConfig;
    std::unordered_map<ParticipantId, RebootId> const safeRebootIds;
  };

  struct alignas(128) GuardedLeaderData {
    ~GuardedLeaderData() = default;
    GuardedLeaderData(LogLeader& self, std::unique_ptr<IReplicatedStateHandle>,
                      LogIndex firstIndex);

    GuardedLeaderData() = delete;
    GuardedLeaderData(GuardedLeaderData const&) = delete;
    GuardedLeaderData(GuardedLeaderData&&) = delete;
    auto operator=(GuardedLeaderData const&) -> GuardedLeaderData& = delete;
    auto operator=(GuardedLeaderData&&) -> GuardedLeaderData& = delete;

    [[nodiscard]] auto prepareAppendEntry(
        std::shared_ptr<FollowerInfo> follower)
        -> std::optional<PreparedAppendEntryRequest>;
    [[nodiscard]] auto prepareAppendEntries()
        -> std::vector<std::optional<PreparedAppendEntryRequest>>;

    [[nodiscard]] auto handleAppendEntriesResponse(
        FollowerInfo& follower, TermIndexPair lastIndex,
        LogIndex currentCommitIndex, LogIndex currentLITK, LogTerm currentTerm,
        futures::Try<AppendEntriesResult>&& res,
        std::chrono::steady_clock::duration latency, MessageId messageId)
        -> std::pair<std::vector<std::optional<PreparedAppendEntryRequest>>,
                     ResolvedPromiseSet>;

    [[nodiscard]] auto checkCommitIndex() -> ResolvedPromiseSet;

    [[nodiscard]] auto collectFollowerStates() const
        -> std::pair<LogIndex, std::vector<algorithms::ParticipantState>>;

    [[nodiscard]] auto updateCommitIndexLeader(
        LogIndex newCommitIndex, std::shared_ptr<QuorumData> quorum)
        -> ResolvedPromiseSet;

    [[nodiscard]] auto getLocalStatistics() const -> LogStatistics;

    [[nodiscard]] auto createAppendEntriesRequest(
        FollowerInfo& follower, TermIndexPair const& lastAvailableIndex) const
        -> std::pair<AppendEntriesRequest, TermIndexPair>;

    [[nodiscard]] auto createSafeRebootIdUpdateCallback(
        LoggerContext logContext) -> IRebootIdCache::Callback;
    void registerSafeRebootIdUpdateCallbackFor(
        std::shared_ptr<replicated_log::LogLeader::FollowerInfo> follower,
        PeerState peerState);
    void registerSafeRebootIdUpdateCallbacks();

    [[nodiscard]] auto updateInnerTermConfig() -> LogIndex;
    [[nodiscard]] auto updateInnerTermConfig(
        std::shared_ptr<InnerTermConfig const> config) -> LogIndex;

    LogLeader& _self;
    std::unordered_map<ParticipantId, std::shared_ptr<FollowerInfo>>
        _follower{};
    WaitForQueue _waitForQueue{};
    WaitForBag _waitForResignQueue;
    std::shared_ptr<QuorumData> _lastQuorum{};
    bool _didResign{false};
    bool _leadershipEstablished{false};
    CommitFailReason _lastCommitFailReason;
    std::unique_ptr<IReplicatedStateHandle> _stateHandle;

    // active - that is currently used to check for committed entries
    std::shared_ptr<InnerTermConfig const> activeInnerTermConfig;
    LogIndex activeInnerConfigLogIndex;
    // committed - latest active config that has committed at least one entry
    // Note that this will be nullptr until leadership is established!
    std::shared_ptr<InnerTermConfig const> committedInnerTermConfig;

    // What would the commit index be if we were to consider only entries that
    // have been synced to disk. Used only for reporting.
    LogIndex _syncCommitIndex;
  };

  LoggerContext const _logContext;
  std::shared_ptr<ReplicatedLogMetrics> const _logMetrics;
  std::shared_ptr<ReplicatedLogGlobalSettings const> const _options;
  std::shared_ptr<IAbstractFollowerFactory> const _followerFactory;
  std::shared_ptr<IScheduler> const _scheduler;
  std::shared_ptr<IRebootIdCache> const _rebootIdCache;
  ParticipantId const _id;
  LogTerm const _currentTerm;
  LogIndex const _firstIndexOfCurrentTerm;
  std::shared_ptr<StorageManager> _storageManager;
  std::shared_ptr<InMemoryLogManager> _inMemoryLogManager;
  std::shared_ptr<CompactionManager> _compactionManager;
  // _localFollower is const after construction
  std::shared_ptr<LocalFollower> _localFollower;
  // make this thread safe in the most simple way possible, wrap everything in
  // a single mutex.
  Guarded<GuardedLeaderData> _guardedLeaderData;

  // Internal insert. Does not trigger async replication. Call
  // triggerAsyncReplication() after to do so.
  [[nodiscard]] auto insertInternal(
      std::variant<LogMetaPayload, LogPayload> payload, bool waitForSync)
      -> LogIndex;

  void updateReleaseIndex(LogIndex doneWithIdx);

  [[nodiscard]] static auto instantiateFollowers(
      LoggerContext const& logContext,
      std::shared_ptr<IAbstractFollowerFactory> followerFactory,
      std::shared_ptr<LocalFollower> const& localFollower,
      TermIndexPair lastEntry,
      std::shared_ptr<agency::ParticipantsConfig const> const&
          participantsConfig)
      -> std::unordered_map<ParticipantId, std::shared_ptr<FollowerInfo>>;

  auto acquireMutex() -> Guard;
  auto acquireMutex() const -> ConstGuard;

  [[nodiscard]] auto updateInnerTermConfig() -> LogIndex;
  [[nodiscard]] auto updateInnerTermConfig(
      std::shared_ptr<InnerTermConfig const> config) -> LogIndex;

  static void executeAppendEntriesRequests(
      std::vector<std::optional<PreparedAppendEntryRequest>> requests,
      std::shared_ptr<ReplicatedLogMetrics> const& logMetrics, IScheduler*);
  static void handleResolvedPromiseSet(
      IScheduler* sched, ResolvedPromiseSet set,
      std::shared_ptr<ReplicatedLogMetrics> const& logMetrics);

  static auto getParticipantsRebootIds(agency::ParticipantsConfig const& config,
                                       IRebootIdCache const& rebootIdCache)
      -> std::unordered_map<ParticipantId, RebootId>;
  auto makeInnerTermConfig(agency::ParticipantsConfig config) const
      -> std::shared_ptr<InnerTermConfig const>;
};

}  // namespace arangodb::replication2::replicated_log
