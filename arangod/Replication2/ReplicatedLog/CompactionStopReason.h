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

#include <cstdint>
#include <iosfwd>
#include <string>
#include <variant>

#include "Inspection/Types.h"

#include "Replication2/ReplicatedLog/LogIndex.h"
#include "Replication2/ReplicatedLog/ParticipantId.h"

namespace arangodb::replication2::replicated_log {

struct CompactionStopReason {
  struct CompactionThresholdNotReached {
    LogIndex nextCompactionAt;
    template<class Inspector>
    friend auto inspect(Inspector& f, CompactionThresholdNotReached& x) {
      return f.object(x).fields(
          f.field("nextCompactionAt", x.nextCompactionAt));
    }
    friend auto operator==(CompactionThresholdNotReached const& left,
                           CompactionThresholdNotReached const& right) noexcept
        -> bool = default;
  };
  struct NotReleasedByStateMachine {
    LogIndex releasedIndex;
    template<class Inspector>
    friend auto inspect(Inspector& f, NotReleasedByStateMachine& x) {
      return f.object(x).fields(f.field("releasedIndex", x.releasedIndex));
    }
    friend auto operator==(NotReleasedByStateMachine const& left,
                           NotReleasedByStateMachine const& right) noexcept
        -> bool = default;
  };
  struct ParticipantMissingEntries {
    ParticipantId who;
    template<class Inspector>
    friend auto inspect(Inspector& f, ParticipantMissingEntries& x) {
      return f.object(x).fields(f.field("who", x.who));
    }
    friend auto operator==(ParticipantMissingEntries const& left,
                           ParticipantMissingEntries const& right) noexcept
        -> bool = default;
  };
  struct LeaderBlocksReleaseEntry {
    LogIndex lowestIndexToKeep;
    template<class Inspector>
    friend auto inspect(Inspector& f, LeaderBlocksReleaseEntry& x) {
      return f.object(x).fields(
          f.field("lowestIndexToKeep", x.lowestIndexToKeep));
    }
    friend auto operator==(LeaderBlocksReleaseEntry const& left,
                           LeaderBlocksReleaseEntry const& right) noexcept
        -> bool = default;
  };

  struct NothingToCompact {
    template<class Inspector>
    friend auto inspect(Inspector& f, NothingToCompact& x) {
      return f.object(x).fields();
    }
    friend auto operator==(NothingToCompact const& left,
                           NothingToCompact const& right) noexcept
        -> bool = default;
  };

  std::variant<CompactionThresholdNotReached, NotReleasedByStateMachine,
               ParticipantMissingEntries, LeaderBlocksReleaseEntry,
               NothingToCompact>
      value;

  friend auto operator==(CompactionStopReason const& left,
                         CompactionStopReason const& right) noexcept
      -> bool = default;

  template<class Inspector>
  friend auto inspect(Inspector& f, CompactionStopReason& x) {
    namespace insp = arangodb::inspection;
    return f.variant(x.value).embedded("reason").alternatives(
        insp::type<CompactionThresholdNotReached>(
            "CompactionThresholdNotReached"),
        insp::type<NotReleasedByStateMachine>("NotReleasedByStateMachine"),
        insp::type<LeaderBlocksReleaseEntry>("LeaderBlocksRelease"),
        insp::type<NothingToCompact>("NothingToCompact"),
        insp::type<ParticipantMissingEntries>("ParticipantMissingEntries"));
  }
};

auto operator<<(std::ostream&, CompactionStopReason const&) -> std::ostream&;
auto to_string(CompactionStopReason const&) -> std::string;

}  // namespace arangodb::replication2::replicated_log
