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

#include "CommitFailReason.h"

#include <Basics/StringUtils.h>
#include <fmt/core.h>

namespace arangodb::replication2::replicated_log {

template<typename... Args>
CommitFailReason::CommitFailReason(std::in_place_t, Args&&... args) noexcept
    : value(std::forward<Args>(args)...) {}

auto CommitFailReason::withNothingToCommit() noexcept -> CommitFailReason {
  return CommitFailReason(std::in_place, NothingToCommit{});
}

auto CommitFailReason::withQuorumSizeNotReached(
    QuorumSizeNotReached::who_type who, TermIndexPair spearhead) noexcept
    -> CommitFailReason {
  return CommitFailReason(std::in_place,
                          QuorumSizeNotReached{std::move(who), spearhead});
}

auto CommitFailReason::withForcedParticipantNotInQuorum(
    ParticipantId who) noexcept -> CommitFailReason {
  return CommitFailReason(std::in_place,
                          ForcedParticipantNotInQuorum{std::move(who)});
}

auto CommitFailReason::withNonEligibleServerRequiredForQuorum(
    NonEligibleServerRequiredForQuorum::CandidateMap candidates) noexcept
    -> CommitFailReason {
  return CommitFailReason(
      std::in_place, NonEligibleServerRequiredForQuorum{std::move(candidates)});
}

auto operator<<(std::ostream& ostream,
                CommitFailReason::QuorumSizeNotReached::ParticipantInfo pInfo)
    -> std::ostream& {
  ostream << "{ ";
  ostream << std::boolalpha;
  if (not pInfo.snapshotAvailable) {
    ostream << "snapshot: " << pInfo.snapshotAvailable << ", ";
  }
  if (pInfo.isAllowedInQuorum) {
    ostream << "isAllowedInQuorum: " << pInfo.isAllowedInQuorum;
  } else {
    ostream << "lastAcknowledgedEntry: " << pInfo.lastAcknowledged;
  }
  ostream << " }";
  return ostream;
}

namespace {
inline constexpr std::string_view NonEligibleNotAllowedInQuorum =
    "notAllowedInQuorum";
inline constexpr std::string_view NonEligibleWrongTerm = "wrongTerm";
inline constexpr std::string_view NonEligibleSnapshotMissing =
    "snapshotMissing";
}  // namespace

auto to_string(
    CommitFailReason::NonEligibleServerRequiredForQuorum::Why why) noexcept
    -> std::string_view {
  using Why = CommitFailReason::NonEligibleServerRequiredForQuorum::Why;
  switch (why) {
    case Why::kNotAllowedInQuorum:
      return NonEligibleNotAllowedInQuorum;
    case Why::kWrongTerm:
      return NonEligibleWrongTerm;
    case Why::kSnapshotMissing:
      return NonEligibleSnapshotMissing;
    default:
      TRI_ASSERT(false);
      return "(unknown)";
  }
}

auto CommitFailReason::withFewerParticipantsThanWriteConcern(
    FewerParticipantsThanWriteConcern fewerParticipantsThanWriteConcern)
    -> CommitFailReason {
  auto result = CommitFailReason();
  result.value = fewerParticipantsThanWriteConcern;
  return result;
}

auto to_string(CommitFailReason const& r) -> std::string {
  struct ToStringVisitor {
    auto operator()(CommitFailReason::NothingToCommit const&) -> std::string {
      return "Nothing to commit";
    }
    auto operator()(CommitFailReason::QuorumSizeNotReached const& reason)
        -> std::string {
      auto stream = std::stringstream();
      stream << "Required quorum size not yet reached. ";
      stream << "The leader's spearhead is at " << reason.spearhead << ". ";
      stream << "Participants who aren't currently contributing to the "
                "spearhead are ";
      // ADL cannot find this operator here.
      arangodb::operator<<(stream, reason.who);
      return stream.str();
    }
    auto operator()(
        CommitFailReason::ForcedParticipantNotInQuorum const& reason)
        -> std::string {
      return "Forced participant not in quorum. Participant " + reason.who;
    }
    auto operator()(
        CommitFailReason::NonEligibleServerRequiredForQuorum const& reason)
        -> std::string {
      auto result =
          std::string{"A non-eligible server is required to reach a quorum:"};
      for (auto const& [pid, why] : reason.candidates) {
        result += basics::StringUtils::concatT(" (", pid, ": ", why, ")");
      }
      return result;
    }
    auto operator()(
        CommitFailReason::FewerParticipantsThanWriteConcern const& reason) {
      return fmt::format(
          "Fewer participants than effective write concern. Have {} ",
          "participants and effectiveWriteConcern={}.", reason.numParticipants,
          reason.effectiveWriteConcern);
    }
  };

  return std::visit(ToStringVisitor{}, r.value);
}

}  // namespace arangodb::replication2::replicated_log
