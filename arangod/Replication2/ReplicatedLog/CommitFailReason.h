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

#include <velocypack/Slice.h>
#include <velocypack/Value.h>
#include <Containers/FlatHashMap.h>

#include "Inspection/Types.h"
#include "Replication2/ReplicatedLog/ParticipantId.h"
#include "Replication2/ReplicatedLog/TermIndexPair.h"

namespace arangodb::replication2::replicated_log {
/*
 * Indicates why the commit index is not increasing as expected.
 * Even though some pending entries might have been committed, unless all
 * pending entries are committed, we say the commit index is behind. This object
 * gives an indication of why might that be.
 */
struct CommitFailReason {
  CommitFailReason() = default;

  struct NothingToCommit {
    friend auto operator==(NothingToCommit const& left,
                           NothingToCommit const& right) noexcept
        -> bool = default;

    template<typename Inspector>
    friend auto inspect(Inspector& f, NothingToCommit& x) {
      return f.object(x).fields();
    }
  };
  struct QuorumSizeNotReached {
    struct ParticipantInfo {
      bool isAllowedInQuorum{};
      bool snapshotAvailable{};
      TermIndexPair lastAcknowledged;
      friend auto operator==(ParticipantInfo const& left,
                             ParticipantInfo const& right) noexcept
          -> bool = default;
      template<typename Inspector>
      friend auto inspect(Inspector& f, ParticipantInfo& x) {
        return f.object(x).fields(
            f.field("isAllowedInQuorum", x.isAllowedInQuorum),
            f.field("snapshotAvailable", x.snapshotAvailable),
            f.field("lastAcknowledged", x.lastAcknowledged));
      }
    };
    using who_type = containers::FlatHashMap<ParticipantId, ParticipantInfo>;

    who_type who;
    TermIndexPair spearhead;
    friend auto operator==(QuorumSizeNotReached const& left,
                           QuorumSizeNotReached const& right) noexcept
        -> bool = default;

    template<typename Inspector>
    friend auto inspect(Inspector& f, QuorumSizeNotReached& x) {
      return f.object(x).fields(f.field("who", x.who),
                                f.field("spearhead", x.spearhead));
    }
  };
  struct ForcedParticipantNotInQuorum {
    ParticipantId who;
    friend auto operator==(ForcedParticipantNotInQuorum const& left,
                           ForcedParticipantNotInQuorum const& right) noexcept
        -> bool = default;
    template<typename Inspector>
    friend auto inspect(Inspector& f, ForcedParticipantNotInQuorum& x) {
      return f.object(x).fields(f.field("who", x.who));
    }
  };
  struct NonEligibleServerRequiredForQuorum {
    enum class Why {
      kNotAllowedInQuorum,
      // WrongTerm might be misleading, because the follower might be in the
      // right term, it just never has acked an entry of the current term.
      kWrongTerm,
      kSnapshotMissing,
    };

    using CandidateMap = std::unordered_map<ParticipantId, Why>;

    CandidateMap candidates;

    friend auto operator==(
        NonEligibleServerRequiredForQuorum const& left,
        NonEligibleServerRequiredForQuorum const& right) noexcept
        -> bool = default;
    template<typename Inspector>
    friend auto inspect(Inspector& f, NonEligibleServerRequiredForQuorum& x) {
      return f.object(x).fields(f.field("candidates", x.candidates));
    }

    template<class Inspector>
    friend auto inspect(Inspector& f, Why& x) {
      return f.enumeration(x).values(
          Why::kNotAllowedInQuorum, "notAllowedInQuorum", Why::kWrongTerm,
          "wrongTerm", Why::kSnapshotMissing, "snapshotMissing");
    }
  };
  struct FewerParticipantsThanWriteConcern {
    std::size_t effectiveWriteConcern{};
    std::size_t numParticipants{};
    friend auto operator==(
        FewerParticipantsThanWriteConcern const& left,
        FewerParticipantsThanWriteConcern const& right) noexcept
        -> bool = default;
    template<typename Inspector>
    friend auto inspect(Inspector& f, FewerParticipantsThanWriteConcern& x) {
      return f.object(x).fields(
          f.field("effectiveWriteConcern", x.effectiveWriteConcern),
          f.field("numParticipants", x.numParticipants));
    }
  };
  std::variant<NothingToCommit, QuorumSizeNotReached,
               ForcedParticipantNotInQuorum, NonEligibleServerRequiredForQuorum,
               FewerParticipantsThanWriteConcern>
      value;

  static auto withNothingToCommit() noexcept -> CommitFailReason;
  static auto withQuorumSizeNotReached(QuorumSizeNotReached::who_type who,
                                       TermIndexPair spearhead) noexcept
      -> CommitFailReason;
  static auto withForcedParticipantNotInQuorum(ParticipantId who) noexcept
      -> CommitFailReason;
  static auto withNonEligibleServerRequiredForQuorum(
      NonEligibleServerRequiredForQuorum::CandidateMap) noexcept
      -> CommitFailReason;
  // This would have too many `std::size_t` arguments to not be confusing,
  // so taking the full object instead.
  static auto withFewerParticipantsThanWriteConcern(
      FewerParticipantsThanWriteConcern) -> CommitFailReason;

  friend auto operator==(CommitFailReason const& left,
                         CommitFailReason const& right) -> bool = default;

  template<typename Inspector>
  friend auto inspect(Inspector& f, CommitFailReason& x) {
    namespace insp = arangodb::inspection;
    return f.variant(x.value).embedded("reason").alternatives(
        insp::type<NothingToCommit>("NothingToCommit"),
        insp::type<QuorumSizeNotReached>("QuorumSizeNotReached"),
        insp::type<ForcedParticipantNotInQuorum>(
            "ForcedParticipantNotInQuorum"),
        insp::type<NonEligibleServerRequiredForQuorum>(
            "NonEligibleServerRequiredForQuorum"),
        insp::type<FewerParticipantsThanWriteConcern>(
            "FewerParticipantsThanWriteConcern"));
  }

 private:
  template<typename... Args>
  explicit CommitFailReason(std::in_place_t, Args&&... args) noexcept;
};

auto to_string(
    CommitFailReason::NonEligibleServerRequiredForQuorum::Why) noexcept
    -> std::string_view;

auto operator<<(std::ostream&,
                CommitFailReason::QuorumSizeNotReached::ParticipantInfo)
    -> std::ostream&;

auto to_string(CommitFailReason const&) -> std::string;

}  // namespace arangodb::replication2::replicated_log
