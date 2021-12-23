
#include "LeaderStateMachine.h"

#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "velocypack/Builder.h"
#include "velocypack/Value.h"

#include <cstdint>
#include <limits>

namespace arangodb::replication2::replicated_state {

auto to_string(LeaderElectionCampaign::Reason reason) -> std::string_view {
  switch (reason) {
    case LeaderElectionCampaign::Reason::OK: {
      return "OK";
    } break;
    case LeaderElectionCampaign::Reason::ServerIll: {
      return "ServerIll";
    } break;
    case LeaderElectionCampaign::Reason::TermNotConfirmed: {
      return "TermNotConfirmed";
    } break;
  }
}
auto operator<<(std::ostream& os, LeaderElectionCampaign::Reason reason)
    -> std::ostream& {
  return os << to_string(reason);
}

void LeaderElectionCampaign::toVelocyPack(VPackBuilder& builder) const {
  auto ob = VPackObjectBuilder(&builder);
  builder.add("numberOKParticipants", VPackValue(numberOKParticipants));

  builder.add(VPackValue("bestTermIndex"));
  bestTermIndex.toVelocyPack(builder);

  {
    auto rb = VPackObjectBuilder(&builder, "reasons");
    for (auto const& [participant, reason] : reasons) {
      builder.add(VPackValue(participant));
      builder.add(VPackValue(to_string(reason)));
    }
  }

  {
    auto eb = VPackArrayBuilder(&builder, "electibleLeaderSet");
    for (auto const& participant : electibleLeaderSet) {
      builder.add(VPackValue(participant));
    }
  }
}

auto to_string(LeaderElectionCampaign const& campaign) -> std::string {
  auto bb = VPackBuilder{};
  campaign.toVelocyPack(bb);
  return bb.toString();
}

auto operator<<(std::ostream& os, Action::ActionType const& action)
    -> std::ostream&;

auto to_string(Action::ActionType action) -> std::string_view {
  switch (action) {
    case Action::ActionType::FailedLeaderElectionAction: {
      return "FailedLeaderElection";
    } break;
    case Action::ActionType::SuccessfulLeaderElectionAction: {
      return "SuccessfulLeaderElection";
    } break;
    case Action::ActionType::UpdateTermAction: {
      return "UpdateTermAction";
    } break;
    case Action::ActionType::ImpossibleCampaignAction: {
      return "ImpossibleCampaignAction";
    } break;
  }
}

auto operator<<(std::ostream& os, Action::ActionType const& action)
    -> std::ostream& {
  return os << to_string(action);
}

auto computeReason(Log::Current::LocalState const& status, bool healthy,
                   LogTerm term) -> LeaderElectionCampaign::Reason {
  if (!healthy) {
    return LeaderElectionCampaign::Reason::ServerIll;
  } else if (term != status.term) {
    return LeaderElectionCampaign::Reason::TermNotConfirmed;
  } else {
    return LeaderElectionCampaign::Reason::OK;
  }
}

auto runElectionCampaign(Log::Current::LocalStates const& states,
                         ParticipantsHealth const& health, LogTerm term)
    -> LeaderElectionCampaign {
  auto campaign = LeaderElectionCampaign{};

  for (auto const& [participant, status] : states) {
    auto reason = computeReason(status, health.isHealthy(participant), term);
    campaign.reasons.emplace(participant, reason);

    if (reason == LeaderElectionCampaign::Reason::OK) {
      campaign.numberOKParticipants += 1;
    }

    if (status.spearhead >= campaign.bestTermIndex) {
      if (status.spearhead != campaign.bestTermIndex) {
        campaign.electibleLeaderSet.clear();
      }
      campaign.electibleLeaderSet.push_back(participant);
      campaign.bestTermIndex = status.spearhead;
    }
  }
  return campaign;
}

auto replicatedLogAction(Log const& log, ParticipantsHealth const& health)
    -> std::unique_ptr<Action> {
  if (log.plan.termSpec.leader) {
    if (health.isHealthy(log.plan.termSpec.leader->serverId) &&
        health.validRebootId(log.plan.termSpec.leader->serverId,
                             log.plan.termSpec.leader->rebootId)) {
      // Current leader is all healthy so nothing to do.
      return nullptr;
    } else {
      auto newTerm = Log::Plan::TermSpecification{};

      newTerm.leader.reset();
      newTerm.term = LogTerm{log.plan.termSpec.term.value + 1};

      return std::make_unique<UpdateTermAction>(newTerm);
    }
  } else {
    // New leader required; we try running an election

    // There aren't enough participants to reach a quorum
    if (log.plan.participants.set.size() + 1 <
        log.plan.termSpec.config.writeConcern) {

        LOG_TOPIC("banana", WARN, Logger::REPLICATION2)
          << "replicated log not enough participants available for leader "
             "election campaign "
          << log.plan.participants.set.size() + 1 << " < "
          << log.plan.termSpec.config.writeConcern;

      return std::make_unique<ImpossibleCampaignAction>();
    }

    auto const& termSpec = log.plan.termSpec;
    auto const& campaign =
        runElectionCampaign(log.current.localStates, health, termSpec.term);

    // This should be taken care of by a test above
    TRI_ASSERT(log.plan.participants.set.size() + 1 >=
               log.plan.termSpec.config.writeConcern);
    auto const requiredNumberOfOKParticipants =
        log.plan.participants.set.size() -
        log.plan.termSpec.config.writeConcern + 1;

    if (campaign.numberOKParticipants >= requiredNumberOfOKParticipants) {
      auto const numElectible = campaign.electibleLeaderSet.size();

      // Something went really wrong: we have enough ok participants, but none
      // of them is electible, or too many of them are (we only support
      // uint16_t::max participants at the moment)
      if (ADB_UNLIKELY(numElectible == 0 ||
                       numElectible > std::numeric_limits<uint16_t>::max())) {
        abortOrThrow(TRI_ERROR_NUMERIC_OVERFLOW,
                     basics::StringUtils::concatT(
                         "Number of participants electible for leadership out "
                         "of range, should be between ",
                         1, " and ", std::numeric_limits<uint16_t>::max(),
                         ", but is ", numElectible),
                     ADB_HERE);
      }

      // We randomly elect on of the electible leaders
      // TODO this can be straightened out a bit
      auto const maxIdx = static_cast<uint16_t>(numElectible - 1);

      auto const& newLeader =
          campaign.electibleLeaderSet.at(RandomGenerator::interval(maxIdx));
      auto const& newLeaderRebootId = health._health.at(newLeader).rebootId;

      auto action = std::make_unique<SuccessfulLeaderElectionAction>();
      action->_campaign = campaign;
      action->_newTerm = Log::Plan::TermSpecification{
          .term = LogTerm{log.plan.termSpec.term.value + 1},
          .leader = Log::Plan::TermSpecification::Leader{
              .serverId = newLeader, .rebootId = newLeaderRebootId}};
      action->_newLeader = newLeader;

      return action;
    } else {
      // Not enough participants were available to form a quorum, so
      // we can't elect a leader

      LOG_TOPIC("banana", WARN, Logger::REPLICATION2)
          << "replicated log not enough participants available for leader "
             "election "
          << campaign.numberOKParticipants << " < "
          << requiredNumberOfOKParticipants;

      auto action = std::make_unique<FailedLeaderElectionAction>();
      action->_campaign = campaign;
      return action;
    }
  }
  return nullptr;
}

}  // namespace arangodb::replication2::replicated_state
