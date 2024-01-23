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

#include <iosfwd>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>

#include "Replication2/ReplicatedLog/LogTerm.h"
#include "Replication2/ReplicatedLog/TermIndexPair.h"
#include "Replication2/ReplicatedLog/Agency/ServerInstanceReference.h"

namespace arangodb::replication2::agency {

struct LogCurrentSupervisionElection {
  // This error code applies to participants, not to
  // the election itself
  enum class ErrorCode {
    OK = 0,
    SERVER_NOT_GOOD = 1,
    TERM_NOT_CONFIRMED = 2,
    SERVER_EXCLUDED = 3,
    SNAPSHOT_MISSING = 4,
  };

  LogTerm term;

  TermIndexPair bestTermIndex;

  // minimum quorum size of voters
  std::size_t participantsRequired{};
  // number of participants that are attending (i.e. reported back during this
  // election)
  std::size_t participantsAttending{};
  // number of participants that are attending and also eligible to vote
  std::size_t participantsVoting{};
  // whether all participants attend this election.
  bool allParticipantsAttending{};
  std::unordered_map<ParticipantId, ErrorCode> detail;
  // set of participants which are attending, eligible, and have the maximum
  // spearhead amongst all attending and eligible participants.
  std::vector<ServerInstanceReference> electibleLeaderSet;

  friend auto operator==(LogCurrentSupervisionElection const&,
                         LogCurrentSupervisionElection const&) noexcept -> bool;
  friend auto operator!=(LogCurrentSupervisionElection const& left,
                         LogCurrentSupervisionElection const& right) noexcept
      -> bool {
    return !(left == right);
  }

  LogCurrentSupervisionElection() = default;
};

auto operator<<(std::ostream&, LogCurrentSupervisionElection const&)
    -> std::ostream&;

auto operator==(LogCurrentSupervisionElection const&,
                LogCurrentSupervisionElection const&) noexcept -> bool;

auto to_string(LogCurrentSupervisionElection::ErrorCode) noexcept
    -> std::string_view;

}  // namespace arangodb::replication2::agency

template<>
struct fmt::formatter<
    arangodb::replication2::agency::LogCurrentSupervisionElection::ErrorCode>
    : formatter<string_view> {
  // parse is inherited from formatter<string_view>.
  template<typename FormatContext>
  auto format(
      arangodb::replication2::agency::LogCurrentSupervisionElection::ErrorCode
          errorCode,
      FormatContext& ctx) const {
    return fmt::format_to(ctx.out(), "{}", to_string(errorCode));
  }
};
