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
#include "CollectionGroupSupervision.h"
#include "Basics/StringUtils.h"
#include "Cluster/Utils/EvenDistribution.h"
#include "Replication2/AgencyCollectionSpecification.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/ParticipantsHealth.h"
#include "Replication2/StateMachines/Document/DocumentStateMachine.h"
#include <random>

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::document::supervision;
namespace {
auto checkReplicatedLogConverged(agency::Log const& log) -> bool {
  if (not log.current or not log.current->supervision) {
    return false;
  }

  return log.current->supervision->targetVersion == log.target.version;
}

auto createLogConfigFromGroupAttributes(
    agency::CollectionGroupTargetSpecification::Attributes const& attributes) {
  return agency::LogTargetConfig{attributes.mutableAttributes.writeConcern,
                                 attributes.mutableAttributes.replicationFactor,
                                 attributes.mutableAttributes.waitForSync};
}

auto getHealthyParticipants(replicated_log::ParticipantsHealth const& health)
    -> std::vector<ParticipantId> {
  std::vector<ParticipantId> servers;

  for (auto const& [p, h] : health._health) {
    if (h.notIsFailed) {
      servers.push_back(p);
    }
  }
  return servers;
}

auto computeEvenDistributionForServers(
    std::size_t numberOfShards, std::size_t replicationFactor,
    replicated_log::ParticipantsHealth const& health) -> EvenDistribution {
  auto servers = getHealthyParticipants(health);
  {
    // TODO reuse random device?
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(servers.begin(), servers.end(), g);
  }

  EvenDistribution distribution{numberOfShards, replicationFactor, {}, false};
  std::unordered_set<ParticipantId> plannedServers;
  distribution.planShardsOnServers(servers, plannedServers);
  return distribution;
}

auto createCollectionGroupTarget(
    agency::CollectionGroupTargetSpecification const& group,
    UniqueIdProvider& uniqid, replicated_log::ParticipantsHealth const& health)
    -> AddCollectionGroupToPlan {
  auto const& attributes = group.attributes;

  auto distribution = computeEvenDistributionForServers(
      attributes.immutableAttributes.numberOfShards,
      attributes.mutableAttributes.replicationFactor, health);

  std::size_t i = 0;
  std::vector<agency::LogTarget> replicatedLogs;
  std::generate_n(
      std::back_inserter(replicatedLogs),
      group.attributes.immutableAttributes.numberOfShards, [&] {
        replicated_state::document::DocumentCoreParameters parameters;

        agency::LogTarget target;
        target.id = LogId{uniqid.next()};
        target.version = 1;
        target.config = createLogConfigFromGroupAttributes(group.attributes);
        target.properties.implementation.type = "document";
        target.properties.implementation.parameters =
            velocypack::serialize(parameters);

        auto participants = distribution.getServersForShardIndex(i++);
        target.leader = participants.getLeader();
        for (auto const& p : participants.servers) {
          target.participants.emplace(p, ParticipantFlags{});
        }

        return target;
      });

  agency::CollectionGroupPlanSpecification spec;
  spec.attributes = group.attributes;
  spec.id = group.id;

  std::transform(
      replicatedLogs.begin(), replicatedLogs.end(),
      std::back_inserter(spec.shardSheaves),
      [](agency::LogTarget const& target) {
        return agency::CollectionGroupPlanSpecification::ShardSheaf{target.id};
      });

  return AddCollectionGroupToPlan{std::move(spec), std::move(replicatedLogs)};
}

auto checkAssociatedReplicatedLogs(
    agency::CollectionGroupTargetSpecification const& target,
    agency::CollectionGroupPlanSpecification const& plan,
    std::map<LogId, agency::Log> const& logs,
    replicated_log::ParticipantsHealth const& health) -> Action {
  // check if replicated logs require an update
  ADB_PROD_ASSERT(plan.shardSheaves.size() ==
                  target.attributes.immutableAttributes.numberOfShards);
  for (auto const& sheaf : plan.shardSheaves) {
    auto const& log = logs.at(sheaf.replicatedLog);
    auto const& wantedConfig =
        createLogConfigFromGroupAttributes(target.attributes);

    if (log.target.config != wantedConfig) {
      // we have to update this replicated log
      return UpdateReplicatedLogConfig{sheaf.replicatedLog, wantedConfig};
    }

    auto expectedReplicationFactor =
        target.attributes.mutableAttributes.replicationFactor;
    auto currentReplicationFactor = log.target.participants.size();
    if (currentReplicationFactor < expectedReplicationFactor) {
      // add a new server to the replicated log
      // find a server not yet used
      auto servers = getHealthyParticipants(health);
      std::erase_if(servers, [&](auto const& server) {
        return log.target.participants.contains(server);
      });

      // TODO pick random server
      if (not servers.empty()) {
        return AddParticipantToLog{log.target.id, servers.front()};
      } else {
        // TODO add report that no server is available
      }
    } else if (currentReplicationFactor > expectedReplicationFactor) {
      // TODO prefer an unhealthy participant over a healthy one
      //  do not pick the current leader
      ADB_PROD_ASSERT(log.target.participants.size() > 1);
      auto const& server = log.target.participants.begin()->first;
      return RemoveParticipantFromLog{log.target.id, server};
    }

    // TODO check add new server
    //  if server failed
  }

  return NoActionRequired{};
}

auto getReplicatedLogLeader(agency::Log const& log)
    -> std::optional<ParticipantId> {
  if (log.plan and log.plan->currentTerm and log.plan->currentTerm->leader) {
    return log.plan->currentTerm->leader->serverId;
  }
  return std::nullopt;
}

auto computeShardList(
    std::map<LogId, agency::Log> const& logs,
    std::vector<agency::CollectionGroupPlanSpecification::ShardSheaf>
        shardSheaves,
    std::vector<ShardID> const& shards) -> PlanShardToServerMapping {
  ADB_PROD_ASSERT(logs.size() == shards.size())
      << "logs.size = " << logs.size() << " shards.size = " << shards.size();
  PlanShardToServerMapping mapping;
  for (std::size_t i = 0; i < shards.size(); i++) {
    ResponsibleServerList servers;
    auto const& log = logs.at(shardSheaves[i].replicatedLog);
    for (auto const& [pid, flags] : log.target.participants) {
      servers.servers.push_back(pid);
    }

    auto leader = getReplicatedLogLeader(log);

    // sort by name, but leader in front
    std::sort(servers.servers.begin(), servers.servers.end(),
              [&](auto const& left, auto const& right) {
                // returns true if left < right
                if (left == leader) {
                  return true;
                }
                return left < right;
              });
    mapping.shards[shards[i]] = std::move(servers);
  }

  return mapping;
}

auto checkCollectionGroupConverged(CollectionGroup const& group) -> Action {
  if (not group.current or
      group.current->supervision.version != group.target.version) {
    // check is all replicated logs have converged
    for (auto const& [id, log] : group.logs) {
      if (not checkReplicatedLogConverged(log)) {
        return NoActionPossible{basics::StringUtils::concatT(
            "replicated log ", id, " not yet converged.")};
      }
    }

    return UpdateConvergedVersion{group.target.version};
  }

  return NoActionRequired{};
}
}  // namespace

auto document::supervision::checkCollectionGroup(
    CollectionGroup const& group, UniqueIdProvider& uniqid,
    replicated_log::ParticipantsHealth const& health) -> Action {
  if (not group.plan.has_value()) {
    // create collection group in plan
    return createCollectionGroupTarget(group.target, uniqid, health);
  }

  // check replicated logs
  if (auto action = checkAssociatedReplicatedLogs(group.target, *group.plan,
                                                  group.logs, health);
      not std::holds_alternative<NoActionRequired>(action)) {
    return action;
  }

  // check that every collection in target is in plan
  for (auto const& [cid, collection] : group.targetCollections) {
    ADB_PROD_ASSERT(group.target.collections.contains(cid));

    if (not group.planCollections.contains(cid)) {
      ADB_PROD_ASSERT(group.plan->collections.contains(cid));

      std::vector<ShardID> shardList;
      std::generate_n(
          std::back_inserter(shardList),
          group.target.attributes.immutableAttributes.numberOfShards,
          [&] { return basics::StringUtils::concatT("s", uniqid.next()); });

      auto mapping =
          computeShardList(group.logs, group.plan->shardSheaves, shardList);
      agency::CollectionPlanSpecification spec{collection, std::move(shardList),
                                               std::move(mapping)};
      return AddCollectionToPlan{std::move(spec)};
    }
  }

  // check that every collection in plan is in target
  for (auto const& [cid, collection] : group.planCollections) {
    ADB_PROD_ASSERT(group.plan->collections.contains(cid));

    auto iter = group.targetCollections.find(cid);
    if (iter == group.targetCollections.end()) {
      return DropCollectionPlan{cid};
    }

    // TODO compare mutable properties and update if necessary

    // TODO remove deprecatedShardMap comparison
    auto expectedShardMap = computeShardList(
        group.logs, group.plan->shardSheaves, collection.shardList);
    if (collection.deprecatedShardMap.shards != expectedShardMap.shards) {
      return UpdateCollectionShardMap{cid, expectedShardMap};
    }
  }

  if (auto action = checkCollectionGroupConverged(group);
      not std::holds_alternative<NoActionRequired>(action)) {
    return action;
  }

  return NoActionRequired{};
}
