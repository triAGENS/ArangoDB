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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Guarded.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/LogCore.h"

#include <iosfwd>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace arangodb::cluster {
struct IFailureOracle;
}
namespace arangodb::replication2::replicated_log {
class LogFollower;
class LogLeader;
struct AbstractFollower;
struct LogCore;
}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::replicated_log {

struct AbstractFollowerFactory {
  virtual ~AbstractFollowerFactory() = default;
  virtual auto constructFollower(ParticipantId const&)
      -> std::shared_ptr<AbstractFollower> = 0;
};

struct IReplicatedLogMethodsBase {
  virtual ~IReplicatedLogMethodsBase() = default;
  virtual auto releaseIndex(LogIndex) -> void = 0;
  virtual auto getLogSnapshot() -> InMemoryLog = 0;
};

struct IReplicatedLogLeaderMethods : IReplicatedLogMethodsBase {
  virtual auto insert(LogPayload) -> LogIndex = 0;
};

struct IReplicatedLogFollowerMethods : IReplicatedLogMethodsBase {
  virtual auto snapshotCompleted() -> Result = 0;
};

// TODO Move to namespace replicate_state (and different file?)
struct IReplicatedStateHandle {
  virtual ~IReplicatedStateHandle() = default;
  virtual auto resign() && noexcept
      -> std::unique_ptr<replicated_log::IReplicatedLogMethodsBase> = 0;
  virtual void leadershipEstablished(
      std::unique_ptr<IReplicatedLogLeaderMethods>) = 0;
  virtual void becomeFollower(
      std::unique_ptr<IReplicatedLogFollowerMethods>) = 0;
  virtual void acquireSnapshot(ServerID leader, LogIndex) = 0;
  virtual void updateCommitIndex(LogIndex) = 0;
  // TODO
  virtual void dropEntries() = 0;  // o.ä. (für waitForSync=false)
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
      std::unique_ptr<LogCore> core,
      std::shared_ptr<ReplicatedLogMetrics> metrics,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options,
      std::shared_ptr<AbstractFollowerFactory> followerFactory,
      LoggerContext const& logContext, agency::ServerInstanceReference myself);

  ~ReplicatedLog();
  ReplicatedLog() = delete;
  ReplicatedLog(ReplicatedLog const&) = delete;
  ReplicatedLog(ReplicatedLog&&) = delete;
  auto operator=(ReplicatedLog const&) -> ReplicatedLog& = delete;
  auto operator=(ReplicatedLog&&) -> ReplicatedLog& = delete;

  [[nodiscard]] auto connect(std::unique_ptr<IReplicatedStateHandle>)
      -> ReplicatedLogConnection;
  void disconnect(ReplicatedLogConnection);

  void updateConfig(agency::LogPlanTermSpecification term,
                    agency::ParticipantsConfig config);

  auto getId() const noexcept -> LogId;
  //  auto getGlobalLogId() const noexcept -> GlobalLogIdentifier const&;
  //  auto becomeLeader(
  //      ParticipantId id, LogTerm term,
  //      std::vector<std::shared_ptr<AbstractFollower>> const& follower,
  //      std::shared_ptr<agency::ParticipantsConfig const> participantsConfig,
  //      std::shared_ptr<cluster::IFailureOracle const> failureOracle)
  //      -> std::shared_ptr<LogLeader>;
  //  auto becomeFollower(ParticipantId id, LogTerm term,
  //                      std::optional<ParticipantId> leaderId)
  //      -> std::shared_ptr<LogFollower>;
  //
  auto getParticipant() const -> std::shared_ptr<ILogParticipant>;
  //
  //  auto getLeader() const -> std::shared_ptr<LogLeader>;
  //  auto getFollower() const -> std::shared_ptr<LogFollower>;
  //
  auto resign() && -> std::unique_ptr<LogCore>;

 private:
  struct GuardedData {
    explicit GuardedData(std::unique_ptr<LogCore> core)
        : core(std::move(core)) {}

    struct LatestConfig {
      explicit LatestConfig(agency::LogPlanTermSpecification term,
                            agency::ParticipantsConfig config)
          : term(std::move(term)), config(std::move(config)) {}
      agency::LogPlanTermSpecification term;
      agency::ParticipantsConfig config;
    };

    std::unique_ptr<LogCore> core = nullptr;
    std::shared_ptr<ILogParticipant> participant = nullptr;
    std::optional<LatestConfig> latest;
    std::shared_ptr<IReplicatedStateHandle> stateHandle;
  };

  void tryBuildParticipant(GuardedData& data);
  void resetParticipant(GuardedData& data);

  LogId const _id;
  LoggerContext const _logContext = LoggerContext(Logger::REPLICATION2);
  std::shared_ptr<ReplicatedLogMetrics> const _metrics;
  std::shared_ptr<ReplicatedLogGlobalSettings const> const _options;
  std::shared_ptr<AbstractFollowerFactory> const _followerFactory;
  agency::ServerInstanceReference const _myself;
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

  ~ReplicatedLogConnection() { disconnect(); }

  void disconnect() {
    if (_log) {
      _log->disconnect(std::move(*this));
    }
  }

 private:
  friend struct ReplicatedLog;
  explicit ReplicatedLogConnection(ReplicatedLog* log) : _log(log) {}

  struct nop {
    template<typename T>
    void operator()(T*) {}
  };

  std::unique_ptr<ReplicatedLog, nop> _log = nullptr;
};

}  // namespace arangodb::replication2::replicated_log
