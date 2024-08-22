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

#include <optional>
#include <unordered_map>
#include <variant>

#include <Inspection/Transformers.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Replication2/ReplicatedLog/types.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"

namespace arangodb::replication2::replicated_log {

enum class ParticipantRole { kUnconfigured, kLeader, kFollower };

/**
 * @brief A minimalist variant of LogStatus, designed to replace FollowerStatus
 * and LeaderStatus where only basic information is needed.
 */
struct QuickLogStatus {
  ParticipantRole role{ParticipantRole::kUnconfigured};
  LocalStateMachineStatus localState{LocalStateMachineStatus::kUnconfigured};
  std::optional<LogTerm> term{};
  LogStatistics local{};
  bool leadershipEstablished{false};
  bool snapshotAvailable{false};
  std::optional<CommitFailReason> commitFailReason{};

  // The following make sense only for a leader.
  std::shared_ptr<agency::ParticipantsConfig const> activeParticipantsConfig{};
  // Note that committedParticipantsConfig will be nullptr until leadership has
  // been established!
  std::shared_ptr<agency::ParticipantsConfig const>
      committedParticipantsConfig{};
  // Note that safeRebootIds will be nullptr until leadership has been
  // established!
  std::shared_ptr<std::unordered_map<ParticipantId, RebootId> const>
      safeRebootIds{};

  std::vector<ParticipantId> followersWithSnapshot{};

  [[nodiscard]] auto getCurrentTerm() const noexcept -> std::optional<LogTerm>;
  [[nodiscard]] auto getLocalStatistics() const noexcept
      -> std::optional<LogStatistics>;
};

auto to_string(ParticipantRole) noexcept -> std::string_view;

struct ParticipantRoleStringTransformer {
  using SerializedType = std::string;
  auto toSerialized(ParticipantRole source, std::string& target) const
      -> inspection::Status;
  auto fromSerialized(std::string const& source, ParticipantRole& target) const
      -> inspection::Status;
};

template<typename Inspector>
auto inspect(Inspector& f, QuickLogStatus& x) {
  auto activeParticipantsConfig = std::shared_ptr<agency::ParticipantsConfig>();
  auto committedParticipantsConfig =
      std::shared_ptr<agency::ParticipantsConfig>();
  auto safeRebootIds =
      std::shared_ptr<std::unordered_map<ParticipantId, RebootId>>();
  if constexpr (!Inspector::isLoading) {
    activeParticipantsConfig = std::make_shared<agency::ParticipantsConfig>();
    committedParticipantsConfig =
        std::make_shared<agency::ParticipantsConfig>();
    safeRebootIds =
        std::make_shared<std::unordered_map<ParticipantId, RebootId>>();
  }
  auto res = f.object(x).fields(
      f.field("role", x.role).transformWith(ParticipantRoleStringTransformer{}),
      f.field("localState", x.localState), f.field("term", x.term),
      f.field("local", x.local),
      f.field("leadershipEstablished", x.leadershipEstablished),
      f.field("snapshotAvailable", x.snapshotAvailable),
      f.field("commitFailReason", x.commitFailReason),
      f.field("followersWithSnapshot", x.followersWithSnapshot),
      f.field("activeParticipantsConfig", activeParticipantsConfig),
      f.field("committedParticipantsConfig", committedParticipantsConfig),
      f.field("safeRebootIds", safeRebootIds));
  if constexpr (Inspector::isLoading) {
    x.activeParticipantsConfig = activeParticipantsConfig;
    x.committedParticipantsConfig = committedParticipantsConfig;
    x.safeRebootIds = safeRebootIds;
  }
  return res;
}

auto to_string(QuickLogStatus const& status) -> std::string;

struct FollowerStatistics : LogStatistics {
  AppendEntriesErrorReason lastErrorReason;
  std::chrono::duration<double, std::milli> lastRequestLatencyMS;
  FollowerState internalState;
  LogIndex nextPrevLogIndex;
  bool snapshotAvailable{false};

  friend auto operator==(FollowerStatistics const& left,
                         FollowerStatistics const& right) noexcept
      -> bool = default;
};

template<class Inspector>
auto inspect(Inspector& f, FollowerStatistics& x) {
  using namespace arangodb;
  return f.object(x).fields(
      f.field(StaticStrings::Spearhead, x.spearHead),
      f.field(StaticStrings::CommitIndex, x.commitIndex),
      f.field(StaticStrings::FirstIndex, x.firstIndex),
      f.field(StaticStrings::ReleaseIndex, x.releaseIndex),
      f.field("nextPrevLogIndex", x.nextPrevLogIndex),
      f.field("lastErrorReason", x.lastErrorReason),
      f.field("snapshotAvailable", x.snapshotAvailable),
      f.field("lastRequestLatencyMS", x.lastRequestLatencyMS)
          .transformWith(inspection::DurationTransformer<
                         std::chrono::duration<double, std::milli>>{}),
      f.field("state", x.internalState));
}

struct CompactionStatus {
  using clock = std::chrono::system_clock;

  struct Compaction {
    clock::time_point time;
    LogRange range;
    std::optional<result::Error> error;

    friend auto operator==(Compaction const& left,
                           Compaction const& right) noexcept -> bool = default;

    template<class Inspector>
    friend auto inspect(Inspector& f, Compaction& x) {
      return f.object(x).fields(
          f.field("time", x.time)
              .transformWith(inspection::TimeStampTransformer{}),
          f.field("range", x.range));
    }
  };

  std::optional<Compaction> lastCompaction;
  std::optional<Compaction> inProgress;
  std::optional<CompactionStopReason> stop;

  friend auto operator==(CompactionStatus const& left,
                         CompactionStatus const& right) noexcept
      -> bool = default;
  template<class Inspector>
  friend auto inspect(Inspector& f, CompactionStatus& x) {
    return f.object(x).fields(f.field("lastCompaction", x.lastCompaction),
                              f.field("stop", x.stop));
  }
};

struct LeaderStatus {
  LogStatistics local;
  LogTerm term;
  LogIndex lowestIndexToKeep;
  LogIndex firstInMemoryIndex;
  LogIndex syncCommitIndex;
  bool leadershipEstablished{false};
  std::unordered_map<ParticipantId, FollowerStatistics> follower;
  // now() - insertTP of last uncommitted entry
  std::chrono::duration<double, std::milli> commitLagMS;
  CommitFailReason lastCommitStatus;
  CompactionStatus compactionStatus;
  agency::ParticipantsConfig activeParticipantsConfig;
  std::optional<agency::ParticipantsConfig> committedParticipantsConfig;
  std::optional<std::unordered_map<ParticipantId, RebootId>> safeRebootIds;

  friend auto operator==(LeaderStatus const& left,
                         LeaderStatus const& right) noexcept -> bool = default;
};

template<class Inspector>
auto inspect(Inspector& f, LeaderStatus& x) {
  auto role = StaticStrings::Leader;
  return f.object(x).fields(
      f.field("role", role), f.field("local", x.local), f.field("term", x.term),
      f.field("lowestIndexToKeep", x.lowestIndexToKeep),
      f.field("firstInMemoryIndex", x.firstInMemoryIndex),
      f.field("syncCommitIndex", x.syncCommitIndex),
      f.field("leadershipEstablished", x.leadershipEstablished),
      f.field("follower", x.follower),
      f.field("commitLagMS", x.commitLagMS)
          .transformWith(inspection::DurationTransformer<
                         std::chrono::duration<double, std::milli>>{}),
      f.field("lastCommitStatus", x.lastCommitStatus),
      f.field("compactionStatus", x.compactionStatus),
      f.field("activeParticipantsConfig", x.activeParticipantsConfig),
      f.field("committedParticipantsConfig", x.committedParticipantsConfig),
      f.field("safeRebootIds", x.safeRebootIds));
}

struct FollowerStatus {
  LogStatistics local;
  std::optional<ParticipantId> leader;
  LogTerm term;
  LogIndex lowestIndexToKeep;
  CompactionStatus compactionStatus;
  bool snapshotAvailable{false};
};

template<class Inspector>
auto inspect(Inspector& f, FollowerStatus& x) {
  auto role = StaticStrings::Follower;
  return f.object(x).fields(f.field("role", role), f.field("local", x.local),
                            f.field("term", x.term),
                            f.field("compactionStatus", x.compactionStatus),
                            f.field("lowestIndexToKeep", x.lowestIndexToKeep),
                            f.field("leader", x.leader),
                            f.field("snapshotAvailable", x.snapshotAvailable));
}

struct UnconfiguredStatus {};

template<class Inspector>
auto inspect(Inspector& f, UnconfiguredStatus& x) {
  auto role = StaticStrings::Unconfigured;
  return f.object(x).fields(f.field("role", role));
}

struct LogStatus {
  using VariantType =
      std::variant<UnconfiguredStatus, LeaderStatus, FollowerStatus>;

  // default constructs as unconfigured status
  LogStatus() = default;
  explicit LogStatus(UnconfiguredStatus) noexcept;
  explicit LogStatus(LeaderStatus) noexcept;
  explicit LogStatus(FollowerStatus) noexcept;

  [[nodiscard]] auto getVariant() const noexcept -> VariantType const&;

  [[nodiscard]] auto getCurrentTerm() const noexcept -> std::optional<LogTerm>;
  [[nodiscard]] auto getLocalStatistics() const noexcept
      -> std::optional<LogStatistics>;

  [[nodiscard]] auto asLeaderStatus() const noexcept -> LeaderStatus const*;
  [[nodiscard]] auto asFollowerStatus() const noexcept -> FollowerStatus const*;

  static auto fromVelocyPack(velocypack::Slice slice) -> LogStatus;
  void toVelocyPack(velocypack::Builder& builder) const;

 private:
  VariantType _variant;
};

auto to_string(LogStatus const& status) -> std::string;

/**
 * @brief Provides a more general view of what's currently going on, without
 * completely relying on the leader.
 */
struct GlobalStatusConnection {
  ErrorCode error{0};
  std::string errorMessage;

  void toVelocyPack(velocypack::Builder&) const;
  static auto fromVelocyPack(velocypack::Slice) -> GlobalStatusConnection;
};

struct GlobalStatus {
  enum class SpecificationSource {
    kLocalCache,
    kRemoteAgency,
  };

  struct ParticipantStatus {
    struct Response {
      using VariantType = std::variant<LogStatus, velocypack::UInt8Buffer>;
      VariantType value;

      void toVelocyPack(velocypack::Builder&) const;
      static auto fromVelocyPack(velocypack::Slice) -> Response;
    };

    GlobalStatusConnection connection;
    std::optional<Response> response;

    void toVelocyPack(velocypack::Builder&) const;
    static auto fromVelocyPack(velocypack::Slice) -> ParticipantStatus;
  };

  struct SupervisionStatus {
    GlobalStatusConnection connection;
    std::optional<agency::LogCurrentSupervision> response;

    void toVelocyPack(velocypack::Builder&) const;
    static auto fromVelocyPack(velocypack::Slice) -> SupervisionStatus;
  };

  struct Specification {
    SpecificationSource source{SpecificationSource::kLocalCache};
    agency::LogPlanSpecification plan;

    void toVelocyPack(velocypack::Builder&) const;
    static auto fromVelocyPack(velocypack::Slice) -> Specification;
  };

  SupervisionStatus supervision;
  std::unordered_map<ParticipantId, ParticipantStatus> participants;
  Specification specification;
  std::optional<ParticipantId> leaderId;
  static auto fromVelocyPack(velocypack::Slice slice) -> GlobalStatus;
  void toVelocyPack(velocypack::Builder& builder) const;
};

auto to_string(GlobalStatus::SpecificationSource source) -> std::string_view;

}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::maintenance {

struct LogStatus {
  arangodb::replication2::replicated_log::QuickLogStatus status;
  arangodb::replication2::agency::ServerInstanceReference server;
};

}  // namespace arangodb::replication2::maintenance
