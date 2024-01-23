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

#include "LogCurrentSupervisionElection.h"

#include <fmt/ranges.h>
#include <fmt/format.h>
#include <Basics/application-exit.h>
#include <Logger/LogMacros.h>

namespace arangodb::replication2::agency {

auto operator<<(std::ostream& ostream, LogCurrentSupervisionElection const& el)
    -> std::ostream& {
  using namespace fmt::literals;
  ostream << fmt::format(
      "Election {{ "
      "term: {term}, "
      "bestTermIndex: {bestTerm}:{bestIndex}, "
      "participantsRequired: {participantsRequired}, "
      "participantsVoting: {participantsVoting}, "
      "electibleLeaderSet: {electibleLeaderSet}, "
      "allParticipantsAttending: {allParticipantsAttending}, "
      "detail: {detail} "
      "}}",
      "term"_a = el.term.value, "bestTerm"_a = el.bestTermIndex.term.value,
      "bestIndex"_a = el.bestTermIndex.index.value,
      "participantsRequired"_a = el.participantsRequired,
      "participantsVoting"_a = el.participantsVoting,
      "electibleLeaderSet"_a = el.electibleLeaderSet,
      // cppcheck-suppress assignBoolToPointer
      "allParticipantsAttending"_a = el.allParticipantsAttending,
      "detail"_a = el.detail);

  return ostream;
}

auto to_string(LogCurrentSupervisionElection::ErrorCode ec) noexcept
    -> std::string_view {
  switch (ec) {
    case LogCurrentSupervisionElection::ErrorCode::OK:
      return "the server is ok";
    case LogCurrentSupervisionElection::ErrorCode::SERVER_NOT_GOOD:
      return "the server is not reported as good in Supervision/Health";
    case LogCurrentSupervisionElection::ErrorCode::TERM_NOT_CONFIRMED:
      return "the server has not (yet) confirmed the current term";
    case LogCurrentSupervisionElection::ErrorCode::SERVER_EXCLUDED:
      return "the server is configured as excluded";
    case LogCurrentSupervisionElection::ErrorCode::SNAPSHOT_MISSING:
      return "the server has no snapshot available";
  }
  LOG_TOPIC("7e572", FATAL, arangodb::Logger::REPLICATION2)
      << "Invalid LogCurrentSupervisionElection::ErrorCode "
      << static_cast<std::underlying_type_t<decltype(ec)>>(ec);
  FATAL_ERROR_ABORT();
}

auto operator==(LogCurrentSupervisionElection const& left,
                LogCurrentSupervisionElection const& right) noexcept -> bool {
  return left.term == right.term &&
         left.participantsVoting == right.participantsVoting &&
         left.participantsRequired == right.participantsRequired &&
         left.detail == right.detail;
}

}  // namespace arangodb::replication2::agency
