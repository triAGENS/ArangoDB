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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Guarded.h"
#include "Cluster/Utils/ShardID.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/Components/ISnapshotManager.h"
#include "Replication2/ReplicatedLog/Components/IStateMetadataTransaction.h"
#include "Replication2/ReplicatedState/StateInterfaces.h"
#include "Replication2/ReplicatedState/StateStatus.h"
#include "Replication2/IScheduler.h"

#include <iosfwd>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace arangodb::replication2::replicated_log {
class LogLeader;
struct AbstractFollower;
struct IRebootIdCache;
}  // namespace arangodb::replication2::replicated_log
namespace arangodb::replication2::maintenance {
struct LogStatus;
}

namespace arangodb::replication2::replicated_log {

struct IAbstractFollowerFactory {
  virtual ~IAbstractFollowerFactory() = default;
  virtual auto constructFollower(ParticipantId const&)
      -> std::shared_ptr<AbstractFollower> = 0;
  virtual auto constructLeaderCommunicator(ParticipantId const&)
      -> std::shared_ptr<replicated_log::ILeaderCommunicator> = 0;
};

struct IReplicatedLogMethodsBase {
  virtual ~IReplicatedLogMethodsBase() = default;
  virtual auto releaseIndex(LogIndex) -> void = 0;
  // no range means everything
  virtual auto getCommittedLogIterator(
      std::optional<LogRange> range = std::nullopt)
      -> std::unique_ptr<LogViewRangeIterator> = 0;
  virtual auto waitFor(LogIndex) -> ILogParticipant::WaitForFuture = 0;
  virtual auto waitForIterator(LogIndex)
      -> ILogParticipant::WaitForIteratorFuture = 0;

  virtual auto beginMetadataTrx()
      -> std::unique_ptr<IStateMetadataTransaction> = 0;
  virtual auto commitMetadataTrx(std::unique_ptr<IStateMetadataTransaction> ptr)
      -> Result = 0;
  virtual auto getCommittedMetadata() const
      -> IStateMetadataTransaction::DataType = 0;
};

struct IReplicatedLogLeaderMethods : IReplicatedLogMethodsBase {
  virtual auto insert(LogPayload, bool waitForSync) -> LogIndex = 0;
};

struct IReplicatedLogFollowerMethods : IReplicatedLogMethodsBase {
  [[nodiscard]] virtual auto snapshotCompleted(std::uint64_t version)
      -> Result = 0;
  [[nodiscard]] virtual auto leaderConnectionEstablished() const -> bool = 0;
  [[nodiscard]] virtual auto checkSnapshotState() const noexcept
      -> replicated_log::SnapshotState = 0;
};

// TODO Move to namespace replicated_state (and different file?)
struct IReplicatedStateHandle {
  virtual ~IReplicatedStateHandle() = default;
  [[nodiscard]] virtual auto resignCurrentState() noexcept
      -> std::unique_ptr<replicated_log::IReplicatedLogMethodsBase> = 0;
  virtual void leadershipEstablished(
      std::unique_ptr<IReplicatedLogLeaderMethods>) = 0;
  virtual void becomeFollower(
      std::unique_ptr<IReplicatedLogFollowerMethods>) = 0;
  virtual void acquireSnapshot(ServerID leader, LogIndex, std::uint64_t) = 0;
  virtual void updateCommitIndex(LogIndex) = 0;
  [[nodiscard]] virtual auto getInternalStatus() const
      -> replicated_state::Status = 0;
};

struct LeaderTermInfo {
  LogTerm term;
  ParticipantId myself;
  std::shared_ptr<agency::ParticipantsConfig const> initialConfig;
};

struct FollowerTermInfo {
  LogTerm term;
  ParticipantId myself;
  std::optional<ParticipantId> leader;
};

struct ParticipantContext {
  LoggerContext loggerContext;
  std::unique_ptr<IReplicatedStateHandle> stateHandle;
  std::shared_ptr<ReplicatedLogMetrics> metrics;
  std::shared_ptr<ReplicatedLogGlobalSettings const> options;
};

struct IParticipantsFactory {
  virtual ~IParticipantsFactory() = default;
  // Exception guarantee: either constructFollower succeeds to create an
  // `ILogFollower`, or `logCore` stays untouched.
  virtual auto constructFollower(
      std::unique_ptr<storage::IStorageEngineMethods>&& methods,
      FollowerTermInfo info, ParticipantContext context)
      -> std::shared_ptr<ILogFollower> = 0;
  // Exception guarantee: either constructLeader succeeds to create an
  // `ILogLeader`, or `logCore` stays untouched.
  virtual auto constructLeader(
      std::unique_ptr<storage::IStorageEngineMethods>&& methods,
      LeaderTermInfo info, ParticipantContext context)
      -> std::shared_ptr<ILogLeader> = 0;
};

struct ReplicatedLogConnection;

/**
 * @brief Container for a replicated log. These are managed by the responsible
 * vocbase. Exactly one instance exists for each replicated log this server is a
 * participant of.
 *
 * It holds a single ILogParticipant; starting with a
 * LogUnconfiguredParticipant, this will usually be either a LogLeader or a
 * LogFollower.
 *
 * The active participant is also responsible for the singular LogCore of this
 * log, providing access to the physical log. The fact that only one LogCore
 * exists, and only one participant has access to it, asserts that only the
 * active instance can write to (or read from) the physical log.
 *
 * ReplicatedLog is responsible for instantiating Participants, and moving the
 * LogCore from the previous active participant to a new one. This happens in
 * becomeLeader and becomeFollower.
 *
 * A mutex must be used to make sure that moving the LogCore from the old to
 * the new participant, and switching the participant pointer, happen
 * atomically.
 */
struct alignas(64) ReplicatedLog {
  explicit ReplicatedLog(
      std::unique_ptr<storage::IStorageEngineMethods> storage,
      std::shared_ptr<ReplicatedLogMetrics> metrics,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options,
      std::shared_ptr<IParticipantsFactory> participantsFactory,
      LoggerContext const& logContext, agency::ServerInstanceReference myself);

  ~ReplicatedLog();
  ReplicatedLog() = delete;
  ReplicatedLog(ReplicatedLog const&) = delete;
  ReplicatedLog(ReplicatedLog&&) = delete;
  auto operator=(ReplicatedLog const&) -> ReplicatedLog& = delete;
  auto operator=(ReplicatedLog&&) -> ReplicatedLog& = delete;

  [[nodiscard]] auto connect(std::unique_ptr<IReplicatedStateHandle>)
      -> ReplicatedLogConnection;
  auto disconnect(ReplicatedLogConnection)
      -> std::unique_ptr<IReplicatedStateHandle>;

  auto updateConfig(agency::LogPlanTermSpecification term,
                    agency::ParticipantsConfig config,
                    agency::ServerInstanceReference myself)
      -> futures::Future<futures::Unit>;

  [[nodiscard]] auto getParticipant() const -> std::shared_ptr<ILogParticipant>;
  [[nodiscard]] auto getQuickStatus() const -> QuickLogStatus;
  [[nodiscard]] auto getStatus() const -> LogStatus;

  [[nodiscard]] auto
  resign() && -> std::unique_ptr<storage::IStorageEngineMethods>;

  [[nodiscard]] auto getMaintenanceLogStatus() const -> maintenance::LogStatus;

 private:
  struct GuardedData {
    explicit GuardedData(
        std::unique_ptr<storage::IStorageEngineMethods> methods,
        agency::ServerInstanceReference myself);

    struct LatestConfig {
      explicit LatestConfig(agency::LogPlanTermSpecification term,
                            agency::ParticipantsConfig config)
          : term(std::move(term)), config(std::move(config)) {}
      agency::LogPlanTermSpecification term;
      agency::ParticipantsConfig config;
    };

    [[nodiscard]] auto getQuickStatus() const -> QuickLogStatus;

    bool resigned{false};
    std::unique_ptr<storage::IStorageEngineMethods> methods;
    std::shared_ptr<ILogParticipant> participant = nullptr;
    agency::ServerInstanceReference _myself;
    std::optional<LatestConfig> latest;
    std::unique_ptr<IReplicatedStateHandle> stateHandle;
  };

  auto tryBuildParticipant(GuardedData& data) -> futures::Future<futures::Unit>;
  void resetParticipant(GuardedData& data);

  LoggerContext const _logContext;
  std::shared_ptr<ReplicatedLogMetrics> const _metrics;
  std::shared_ptr<ReplicatedLogGlobalSettings const> const _options;
  std::shared_ptr<IParticipantsFactory> const _participantsFactory;
  Guarded<GuardedData> _guarded;
};

struct ReplicatedLogConnection {
  ReplicatedLogConnection() = default;
  ReplicatedLogConnection(ReplicatedLogConnection const&) = delete;
  ReplicatedLogConnection(ReplicatedLogConnection&&) noexcept = default;
  auto operator=(ReplicatedLogConnection const&)
      -> ReplicatedLogConnection& = delete;
  auto operator=(ReplicatedLogConnection&&) noexcept
      -> ReplicatedLogConnection& = default;

  ~ReplicatedLogConnection();

  void disconnect();

 private:
  friend struct ReplicatedLog;
  explicit ReplicatedLogConnection(ReplicatedLog* log);

  struct nop {
    template<typename T>
    void operator()(T*) {}
  };

  std::unique_ptr<ReplicatedLog, nop> _log = nullptr;
};

struct DefaultParticipantsFactory : IParticipantsFactory {
  explicit DefaultParticipantsFactory(
      std::shared_ptr<IAbstractFollowerFactory> followerFactory,
      std::shared_ptr<IScheduler> scheduler,
      std::shared_ptr<IRebootIdCache> rebootIdCache);
  auto constructFollower(std::unique_ptr<storage::IStorageEngineMethods>&&,
                         FollowerTermInfo info, ParticipantContext context)
      -> std::shared_ptr<ILogFollower> override;
  auto constructLeader(std::unique_ptr<storage::IStorageEngineMethods>&&,
                       LeaderTermInfo info, ParticipantContext context)
      -> std::shared_ptr<ILogLeader> override;

  std::shared_ptr<IAbstractFollowerFactory> followerFactory;
  std::shared_ptr<IScheduler> scheduler;
  std::shared_ptr<IRebootIdCache> rebootIdCache;
};

}  // namespace arangodb::replication2::replicated_log
