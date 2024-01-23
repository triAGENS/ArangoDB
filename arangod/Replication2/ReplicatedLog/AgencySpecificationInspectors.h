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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////
#pragma once

// TODO - move these inspectors to their respective types and remove this file

#include "AgencyLogSpecification.h"

#include "Inspection/Transformers.h"

namespace arangodb::replication2::agency {

namespace static_strings {
auto constexpr CommittedParticipantsConfig =
    std::string_view{"committedParticipantsConfig"};
auto constexpr ParticipantsConfig = std::string_view{"participantsConfig"};
auto constexpr BestTermIndex = std::string_view{"bestTermIndex"};
auto constexpr ParticipantsRequired = std::string_view{"participantsRequired"};
auto constexpr ParticipantsAttending =
    std::string_view{"participantsAttending"};
auto constexpr ParticipantsVoting = std::string_view{"participantsVoting"};
auto constexpr AllParticipantsAttending =
    std::string_view{"allParticipantsAttending"};
auto constexpr Details = std::string_view{"details"};
auto constexpr ElectibleLeaderSet = std::string_view{"electibleLeaderSet"};
auto constexpr Error = std::string_view{"error"};
auto constexpr StatusReport = std::string_view{"StatusReport"};
auto constexpr LeadershipEstablished =
    std::string_view{"leadershipEstablished"};
auto constexpr CommitStatus = std::string_view{"commitStatus"};
auto constexpr Supervision = std::string_view{"supervision"};
auto constexpr Leader = std::string_view{"leader"};
auto constexpr SafeRebootIds = std::string_view{"safeRebootIds"};
auto constexpr TargetVersion = std::string_view{"targetVersion"};
auto constexpr Version = std::string_view{"version"};
auto constexpr Actions = std::string_view{"actions"};
auto constexpr MaxActionsTraceLength =
    std::string_view{"maxActionsTraceLength"};
auto constexpr Code = std::string_view{"code"};
auto constexpr Message = std::string_view{"message"};
auto constexpr LastTimeModified = std::string_view{"lastTimeModified"};
auto constexpr Owner = std::string_view{"owner"};
auto constexpr AssumedWriteConcern = std::string_view{"assumedWriteConcern"};
auto constexpr AssumedWaitForSync = std::string_view{"assumedWaitForSync"};
}  // namespace static_strings

template<class Inspector>
auto inspect(Inspector& f, LogPlanSpecification& x) {
  return f.object(x).fields(
      f.field(StaticStrings::Id, x.id),
      f.field(StaticStrings::CurrentTerm, x.currentTerm),
      f.field(static_strings::Owner, x.owner),
      f.field("properties", x.properties),
      f.field(static_strings::ParticipantsConfig, x.participantsConfig));
};

template<class Inspector>
auto inspect(Inspector& f, LogCurrentLocalState& x) {
  return f.object(x).fields(f.field(StaticStrings::Term, x.term),
                            f.field(StaticStrings::Spearhead, x.spearhead),
                            f.field("state", x.state),
                            f.field("snapshotAvailable", x.snapshotAvailable),
                            f.field("rebootId", x.rebootId));
}

template<typename Enum>
struct EnumStruct {
  static_assert(std::is_enum_v<Enum>);

  EnumStruct() : code{0}, message{} {};
  EnumStruct(Enum e)
      : code(static_cast<std::underlying_type_t<Enum>>(e)),
        message(to_string(e)){};

  [[nodiscard]] auto get() const -> Enum { return static_cast<Enum>(code); }

  std::underlying_type_t<Enum> code;
  std::string message;
};

template<class Inspector, typename Enum>
auto inspect(Inspector& f, EnumStruct<Enum>& x) {
  return f.object(x).fields(f.field(static_strings::Code, x.code),
                            f.field(static_strings::Message, x.message));
};

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervisionElection::ErrorCode& x) {
  if constexpr (Inspector::isLoading) {
    auto v = EnumStruct<LogCurrentSupervisionElection::ErrorCode>();
    auto res = f.apply(v);
    if (res.ok()) {
      x = v.get();
    }
    return res;
  } else {
    auto v = EnumStruct<LogCurrentSupervisionElection::ErrorCode>(x);
    return f.apply(v);
  }
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervisionElection& x) {
  return f.object(x).fields(
      f.field(StaticStrings::Term, x.term),
      f.field(static_strings::BestTermIndex, x.bestTermIndex),
      f.field(static_strings::ParticipantsRequired, x.participantsRequired),
      f.field(static_strings::ParticipantsAttending, x.participantsAttending),
      f.field(static_strings::ParticipantsVoting, x.participantsVoting),
      f.field(static_strings::AllParticipantsAttending,
              x.allParticipantsAttending),
      f.field(static_strings::Details, x.detail),
      f.field(static_strings::ElectibleLeaderSet, x.electibleLeaderSet));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::TargetLeaderInvalid& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::TargetLeaderExcluded& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f,
             LogCurrentSupervision::TargetLeaderSnapshotMissing& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::TargetLeaderFailed& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f,
             LogCurrentSupervision::TargetNotEnoughParticipants& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f,
             LogCurrentSupervision::WaitingForConfigCommitted& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::LeaderElectionImpossible& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f,
             LogCurrentSupervision::LeaderElectionOutOfBounds& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f,
             LogCurrentSupervision::LeaderElectionQuorumNotReached& x) {
  return f.object(x).fields(f.field("election", x.election));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::LeaderElectionSuccess& x) {
  return f.object(x).fields(f.field("election", x.election));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::SwitchLeaderFailed& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::PlanNotAvailable& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::CurrentNotAvailable& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision::StatusMessage& x) {
  namespace insp = arangodb::inspection;
  return f.variant(x)
      .qualified("type", "detail")
      .alternatives(
          insp::type<LogCurrentSupervision::TargetLeaderInvalid>(
              LogCurrentSupervision::TargetLeaderInvalid::code),
          insp::type<LogCurrentSupervision::TargetLeaderExcluded>(
              LogCurrentSupervision::TargetLeaderExcluded::code),
          insp::type<LogCurrentSupervision::TargetLeaderSnapshotMissing>(
              LogCurrentSupervision::TargetLeaderSnapshotMissing::code),
          insp::type<LogCurrentSupervision::TargetLeaderFailed>(
              LogCurrentSupervision::TargetLeaderFailed::code),
          insp::type<LogCurrentSupervision::TargetNotEnoughParticipants>(
              LogCurrentSupervision::TargetNotEnoughParticipants::code),
          insp::type<LogCurrentSupervision::WaitingForConfigCommitted>(
              LogCurrentSupervision::WaitingForConfigCommitted::code),
          insp::type<LogCurrentSupervision::LeaderElectionImpossible>(
              LogCurrentSupervision::LeaderElectionImpossible::code),
          insp::type<LogCurrentSupervision::LeaderElectionOutOfBounds>(
              LogCurrentSupervision::LeaderElectionOutOfBounds::code),
          insp::type<LogCurrentSupervision::LeaderElectionQuorumNotReached>(
              LogCurrentSupervision::LeaderElectionQuorumNotReached::code),
          insp::type<LogCurrentSupervision::LeaderElectionSuccess>(
              LogCurrentSupervision::LeaderElectionSuccess::code),
          insp::type<LogCurrentSupervision::SwitchLeaderFailed>(
              LogCurrentSupervision::SwitchLeaderFailed::code),
          insp::type<LogCurrentSupervision::PlanNotAvailable>(
              LogCurrentSupervision::PlanNotAvailable::code),
          insp::type<LogCurrentSupervision::CurrentNotAvailable>(
              LogCurrentSupervision::CurrentNotAvailable::code));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrentSupervision& x) {
  return f.object(x).fields(
      f.field(static_strings::AssumedWriteConcern, x.assumedWriteConcern)
          .fallback(static_cast<size_t>(1)),
      f.field(static_strings::AssumedWaitForSync, x.assumedWaitForSync),
      f.field(static_strings::TargetVersion, x.targetVersion),
      f.field(static_strings::StatusReport, x.statusReport),
      f.field(static_strings::LastTimeModified, x.lastTimeModified)
          .transformWith(inspection::TimeStampTransformer{}));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrent::Leader& x) {
  return f.object(x).fields(
      f.field(StaticStrings::ServerId, x.serverId),
      f.field(StaticStrings::Term, x.term),
      f.field(static_strings::CommittedParticipantsConfig,
              x.committedParticipantsConfig),
      f.field(static_strings::LeadershipEstablished, x.leadershipEstablished)
          .fallback(false),
      f.field(static_strings::CommitStatus, x.commitStatus));
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrent::ActionDummy& x) {
  if constexpr (Inspector::isLoading) {
    x = LogCurrent::ActionDummy{};
  } else {
  }
  return arangodb::inspection::Status::Success{};
}

template<class Inspector>
auto inspect(Inspector& f, LogCurrent& x) {
  return f.object(x).fields(
      f.field(StaticStrings::LocalStatus, x.localState)
          .fallback(std::unordered_map<ParticipantId, LogCurrentLocalState>{}),
      f.field(static_strings::Supervision, x.supervision),
      f.field(static_strings::Leader, x.leader),
      f.field(static_strings::SafeRebootIds, x.safeRebootIds)
          .fallback(std::unordered_map<ParticipantId, RebootId>{}),
      f.field(static_strings::Actions, x.actions)
          .fallback(std::vector<LogCurrent::ActionDummy>{}));
}

template<class Inspector>
auto inspect(Inspector& f, LogTarget::Supervision& x) {
  return f.object(x).fields(
      f.field(static_strings::MaxActionsTraceLength, x.maxActionsTraceLength));
}

template<class Inspector>
auto inspect(Inspector& f, LogTarget& x) {
  return f.object(x).fields(
      f.field(StaticStrings::Id, x.id),
      f.field(StaticStrings::Participants, x.participants)
          .fallback(ParticipantsFlagsMap{}),
      f.field(StaticStrings::Properties, x.properties),
      f.field(StaticStrings::Config, x.config),
      f.field(StaticStrings::Leader, x.leader),
      f.field(static_strings::Version, x.version),
      f.field(static_strings::Owner, x.owner),
      f.field(static_strings::Supervision, x.supervision));
}

}  // namespace arangodb::replication2::agency
