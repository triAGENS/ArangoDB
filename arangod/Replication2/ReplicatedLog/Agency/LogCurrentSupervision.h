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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <chrono>
#include <iosfwd>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "Replication2/ReplicatedLog/Agency/LogCurrentSupervisionElection.h"

namespace arangodb::replication2::agency {

struct LogCurrentSupervision {
  using clock = std::chrono::system_clock;

  struct TargetLeaderInvalid {
    static constexpr std::string_view code = "TargetLeaderInvalid";
    friend auto operator==(TargetLeaderInvalid const& s,
                           TargetLeaderInvalid const& s2) noexcept
        -> bool = default;
  };
  struct TargetLeaderExcluded {
    static constexpr std::string_view code = "TargetLeaderExcluded";
    friend auto operator==(TargetLeaderExcluded const& s,
                           TargetLeaderExcluded const& s2) noexcept
        -> bool = default;
  };
  struct TargetLeaderSnapshotMissing {
    static constexpr std::string_view code = "TargetLeaderSnapshotMissing";
    friend auto operator==(TargetLeaderSnapshotMissing const& s,
                           TargetLeaderSnapshotMissing const& s2) noexcept
        -> bool = default;
  };
  struct TargetLeaderFailed {
    static constexpr std::string_view code = "TargetLeaderFailed";
    friend auto operator==(TargetLeaderFailed const& s,
                           TargetLeaderFailed const& s2) noexcept
        -> bool = default;
  };
  struct TargetNotEnoughParticipants {
    static constexpr std::string_view code = "TargetNotEnoughParticipants";
    friend auto operator==(TargetNotEnoughParticipants const& s,
                           TargetNotEnoughParticipants const& s2) noexcept
        -> bool = default;
  };
  struct WaitingForConfigCommitted {
    static constexpr std::string_view code = "WaitingForConfigCommitted";
    friend auto operator==(WaitingForConfigCommitted const& s,
                           WaitingForConfigCommitted const& s2) noexcept
        -> bool = default;
  };
  struct LeaderElectionImpossible {
    static constexpr std::string_view code = "LeaderElectionImpossible";
    friend auto operator==(LeaderElectionImpossible const& s,
                           LeaderElectionImpossible const& s2) noexcept
        -> bool = default;
  };
  struct LeaderElectionOutOfBounds {
    static constexpr std::string_view code = "LeaderElectionOutOfBounds";
    friend auto operator==(LeaderElectionOutOfBounds const& s,
                           LeaderElectionOutOfBounds const& s2) noexcept
        -> bool = default;
  };
  struct LeaderElectionQuorumNotReached {
    static constexpr std::string_view code = "LeaderElectionQuorumNotReached";
    LogCurrentSupervisionElection election;
    friend auto operator==(LeaderElectionQuorumNotReached const& s,
                           LeaderElectionQuorumNotReached const& s2) noexcept
        -> bool = default;
  };
  struct LeaderElectionSuccess {
    static constexpr std::string_view code = "LeaderElectionSuccess";
    LogCurrentSupervisionElection election;
    friend auto operator==(LeaderElectionSuccess const& s,
                           LeaderElectionSuccess const& s2) noexcept
        -> bool = default;
  };
  struct SwitchLeaderFailed {
    static constexpr std::string_view code = "SwitchLeaderFailed";
    friend auto operator==(SwitchLeaderFailed const& s,
                           SwitchLeaderFailed const& s2) noexcept
        -> bool = default;
  };
  struct PlanNotAvailable {
    static constexpr std::string_view code = "PlanNotAvailable";
    friend auto operator==(PlanNotAvailable const& s,
                           PlanNotAvailable const& s2) noexcept
        -> bool = default;
  };
  struct CurrentNotAvailable {
    static constexpr std::string_view code = "CurrentNotAvailable";
    friend auto operator==(CurrentNotAvailable const& s,
                           CurrentNotAvailable const& s2) noexcept
        -> bool = default;
  };

  using StatusMessage =
      std::variant<TargetLeaderInvalid, TargetLeaderExcluded,
                   TargetLeaderSnapshotMissing, TargetLeaderFailed,
                   TargetNotEnoughParticipants, WaitingForConfigCommitted,
                   LeaderElectionImpossible, LeaderElectionOutOfBounds,
                   LeaderElectionQuorumNotReached, LeaderElectionSuccess,
                   SwitchLeaderFailed, PlanNotAvailable, CurrentNotAvailable>;

  using StatusReport = std::vector<StatusMessage>;

  // TODO / FIXME: This prevents assumedWriteConcern from
  // being initialised to 0, which would prevent any progress
  // at all and a broken log.
  // Under normal operation assumedWriteConcern is set to the
  // first effectiveWriteConcern that is calculated on creation
  // of the log.
  size_t assumedWriteConcern{1};
  bool assumedWaitForSync{false};
  std::optional<uint64_t> targetVersion;
  std::optional<StatusReport> statusReport;
  std::optional<clock::time_point> lastTimeModified;

  LogCurrentSupervision() = default;
  friend auto operator==(LogCurrentSupervision const& s,
                         LogCurrentSupervision const& s2) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2::agency
