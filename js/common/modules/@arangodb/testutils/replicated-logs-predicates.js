/*jshint strict: true */
/*global print*/
'use strict';
// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
// / Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
// /
// / Licensed under the Business Source License 1.1 (the "License");
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     https://github.com/arangodb/arangodb/blob/devel/LICENSE
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
/// @author Tobias Gödderz
// //////////////////////////////////////////////////////////////////////////////

const LH = require("@arangodb/testutils/replicated-logs-helper");
const _ = require("lodash");

const replicatedLogIsReady = function (database, logId, term, participants, leader) {
  return function () {
    const {current, plan} = LH.readReplicatedLogAgency(database, logId);
    if (plan === undefined) {
      return Error("plan not yet defined");
    }
    if (current === undefined) {
      return Error("current not yet defined");
    }
    const electionTerm = !plan.currentTerm.hasOwnProperty('leader');
    // If there's no leader, followers will stay in "Connecting".
    const readyState = electionTerm ? 'Connecting' : 'ServiceOperational';

    for (const srv of participants) {
      if (!current.localStatus || !current.localStatus[srv]) {
        return Error(`Participant ${srv} has not yet reported to current.`);
      }
      if (current.localStatus[srv].term < term) {
        return Error(`Participant ${srv} has not yet acknowledged the current term; ` +
            `found = ${current.localStatus[srv].term}, expected = ${term}.`);
      }
      if (current.localStatus[srv].state !== readyState) {
        return Error(`Participant ${srv} state not yet ready, found  ${current.localStatus[srv].state}` +
            `, expected = "${readyState}".`);
      }
    }

    if (leader !== undefined) {
      if (!current.leader) {
        return Error("Leader has not yet established its term");
      }
      if (current.leader.serverId !== leader) {
        return Error(`Wrong leader in current; found = ${current.leader.serverId}, expected = ${leader}`);
      }
      if (current.leader.term < term) {
        return Error(`Leader has not yet confirmed the term; found = ${current.leader.term}, expected = ${term}`);
      }
      if (!current.leader.leadershipEstablished) {
        return Error("Leader has not yet established its leadership");
      }
    }
    return true;
  };
};

const replicatedLogIsGone = function (database, logId) {
  return function () {
    let {target, plan, current} = LH.readReplicatedLogAgency(database, logId);
    if (target !== undefined) {
      return Error("target still set");
    }
    if (plan !== undefined) {
      return Error("plan still set");
    }
    if (current !== undefined) {
      return Error("current still set");
    }
    return true;
  };
};

const replicatedLogLeaderEstablished = function (database, logId, term, participants) {
  return function () {
    let {plan, current} = LH.readReplicatedLogAgency(database, logId);
    if (current === undefined) {
      return Error("current not yet defined");
    }
    if (plan === undefined) {
      // This may seem strange, but it's actually needed. Due to supervision logging actions to Current,
      // we can end up with an entry in Current, before the Plan is created.
      return Error("plan not yet defined");
    }

    for (const srv of participants) {
      if (!current.localStatus || !current.localStatus[srv]) {
        return Error(`Participant ${srv} has not yet reported to current.`);
      }
      if (term !== undefined && current.localStatus[srv].term < term) {
        return Error(`Participant ${srv} has not yet acknowledged the current term; ` +
            `found = ${current.localStatus[srv].term}, expected = ${term}.`);
      }
    }

    if (term === undefined) {
      if (!plan.currentTerm) {
        return Error(`No term in plan`);
      }
      term = plan.currentTerm.term;
    }

    if (!current.leader || current.leader.term < term) {
      return Error("Leader has not yet established its term");
    }
    if (!current.leader.leadershipEstablished) {
      return Error("Leader has not yet established its leadership");
    }

    return true;
  };
};

const replicatedStateAccessible = function (database, logId) {
  return function () {
    let {target, plan, current} = LH.readReplicatedLogAgency(database, logId);
    if (current === undefined) {
      return Error("current not yet defined");
    }

    if (!plan.currentTerm || !plan.currentTerm.leader) {
      return Error("No term with leader present");
    }
    const expectedTerm = plan.currentTerm.term;
    const leaderId = plan.currentTerm.leader.serverId;


    const targetLeader = target.leader;
    if (targetLeader && targetLeader !== leaderId) {
      return Error(`Leader in target is ${targetLeader}, but current in Plan is ${leaderId}`);
    }

    const expectedRebootId = plan.currentTerm.leader.rebootId;
    const actualRebootId = LH.getServerRebootId(leaderId);
    if (expectedRebootId !== actualRebootId) {
      return Error(`leader reboot id is ${actualRebootId}, expected ${expectedRebootId}`);
    }

    if (!current.leader || current.leader.term !== expectedTerm) {
      return Error("Leader has not yet established its term");
    }
    if (!current.leader.leadershipEstablished) {
      return Error("Leader has not yet established its leadership");
    }

    // check if leader recovery is completed
    const leaderLocal = current.localStatus[leaderId];
    if (leaderLocal.state !== "ServiceOperational" && leaderLocal.term === expectedTerm) {
      return Error(`Leader state is ${current.localStatus[leaderId].state}.`);
    }

    return true;
  };
};

const allReplicatedStatesAccessible = function (database) {
  return function () {
    let plan = LH.readAgencyValueAt(`Plan/ReplicatedLogs/${database}`);
    for (const logId of Object.keys(plan)) {
      const result = replicatedStateAccessible(database, logId)();
      if (result !== true) {
        return result;
      }
    }
    return true;
  };
};

const replicatedStateConverged = function (database, logId) {
  const version = LH.increaseTargetVersion(database, logId);
  return replicatedLogTargetVersion(database, logId, version);
};

const allReplicatedStatesConverged = function (database) {
  let versions = {};
  const plan = LH.readAgencyValueAt(`Plan/ReplicatedLogs/${database}`);
  for (const logId of Object.keys(plan)) {
    versions[logId] = LH.increaseTargetVersion(database, logId);
  }
  return function () {
    const plan = LH.readAgencyValueAt(`Plan/ReplicatedLogs/${database}`);
    for (const logId of Object.keys(plan)) {
      const result = replicatedLogTargetVersion(database, logId, versions[logId])();
      if (result !== true) {
        return result;
      }
    }
    return true;
  };
};

const allServersHealthy = function () {
  return function () {
    for (const server of LH.dbservers) {
      const health = LH.getServerHealth(server);
      if (health !== "GOOD") {
        return Error(`${server} is ${health}`);
      }
    }

    return true;
  };
};

const checkServerHealth = function (serverId, value) {
  return function () {
    if (value === LH.getServerHealth(serverId)) {
      return true;
    }
    return Error(`${serverId} is not ${value}`);
  };
};

const serverHealthy = (serverId) => checkServerHealth(serverId, "GOOD");
const serverFailed = (serverId) => checkServerHealth(serverId, "FAILED");

const replicatedLogParticipantsFlag = function (database, logId, flags, generation = undefined) {
  return function () {
    let {current} = LH.readReplicatedLogAgency(database, logId);
    if (current === undefined) {
      return Error("current not yet defined");
    }
    if (!current.leader) {
      return Error("Leader has not yet established its term");
    }
    if (!current.leader.committedParticipantsConfig) {
      return Error("Leader has not yet committed any participants config");
    }
    if (generation !== undefined) {
      if (current.leader.committedParticipantsConfig.generation < generation) {
        return Error("Leader has not yet acked new generation; "
            + `found ${current.leader.committedParticipantsConfig.generation}, expected = ${generation}`);
      }
    }

    const participants = current.leader.committedParticipantsConfig.participants;
    for (const [p, v] of Object.entries(flags)) {
      if (v === null) {
        if (participants[p] !== undefined) {
          return Error(`Entry for server ${p} still present in participants flags`);
        }
      } else {
        if (!_.isEqual(v, participants[p])) {
          return Error(`Flags for participant ${p} are not as expected: ${JSON.stringify(v)} vs. ${JSON.stringify(participants[p])}`);
        }
      }
    }

    return true;
  };
};

const replicatedLogTargetVersion = function (database, logId, version) {
  return function () {
    let {current} = LH.readReplicatedLogAgency(database, logId);

    if (current === undefined) {
      return Error(`current not yet defined`);
    }
    if (!current.supervision) {
      return Error(`supervision not yet reported to current`);
    }
    if (!current.supervision.targetVersion) {
      return Error(`no version reported in current by supervision`);
    }
    if (current.supervision.targetVersion !== version) {
      return Error(`found version ${current.supervision.targetVersion}, expected ${version}`);
    }
    return true;
  };
};

const replicatedLogLeaderTargetIs = function (database, stateId, expectedLeader) {
  return function () {
    const currentLeader = LH.getReplicatedLogLeaderTarget(database, stateId);
    if (currentLeader === expectedLeader) {
      return true;
    } else {
      return new Error(`Expected log leader to switch to ${expectedLeader}, but is still ${currentLeader}`);
    }
  };
};

const replicatedLogLeaderPlanIs = function (database, stateId, expectedLeader) {
  return function () {
    const {leader: currentLeader} = LH.getReplicatedLogLeaderPlan(database, stateId);
    if (currentLeader === expectedLeader) {
      return true;
    } else {
      return new Error(`Expected log leader to switch to ${expectedLeader}, but is still ${currentLeader}`);
    }
  };
};

const replicatedLogLeaderPlanChanged = function (database, stateId, oldLeader) {
  return function () {
    const leaderPlan = LH.getReplicatedLogLeaderPlan(database, stateId, true);
    if (leaderPlan instanceof Error) {
      return leaderPlan;
    }
    if (leaderPlan.leader !== oldLeader) {
      return true;
    } else {
      return new Error(`Expected log leader to switch from ${oldLeader}, but is still the same`);
    }
  };
};

const replicatedLogLeaderCommitFail = function (database, logId, expected) {
  return function () {
    let {current} = LH.readReplicatedLogAgency(database, logId);
    if (current === undefined) {
      return Error("current not yet defined");
    }
    if (!current.leader) {
      return Error("Leader has not yet established its term");
    }

    const status = current.leader.commitStatus;
    if (expected === undefined) {
      if (status !== undefined) {
        return Error(`CommitStatus not yet cleared, current-value = ${status.reason}`);
      }
    } else {
      if (status === undefined) {
        return Error("CommitStatus not yet set.");
      } else if (status.reason !== expected) {
        return Error(`CommitStatus not as expected, found ${status.reason}; expected ${expected}`);
      }
    }

    return true;
  };
};

const replicatedLogParticipantGeneration = function (database, logId, generation) {
  return function () {
    let {current} = LH.readReplicatedLogAgency(database, logId);
    if (current === undefined) {
      return Error("current not yet defined");
    }
    if (!current.leader) {
      return Error("Leader has not yet established its term");
    }
    if (!current.leader.committedParticipantsConfig) {
      return Error("Leader has not yet committed any participants config");
    }
    if (current.leader.committedParticipantsConfig.generation < generation) {
      return Error("Leader has not yet acked new generation; "
          + `found ${current.leader.committedParticipantsConfig.generation}, expected = ${generation}`);
    }

    return true;
  };
};

const replicatedLogReplicationCompleted = function (database, logId) {
  return function () {
    const result = LH.getReplicatedLogLeaderPlan(database, logId, true);
    if (result instanceof Error) {
      return result;
    }
    const status = LH.getLocalStatus(result.leader, database, logId);
    const local = status.local;

    for (const [pid, follower] of Object.entries(status.follower)) {
      if (follower.spearhead < local.spearhead || follower.commitIndex < local.commitIndex) {
        return Error(`Replication to ${pid} not yet completed, spearhead = ${follower.spearhead}/${local.spearhead}, ` +
            `commit-index = ${follower.commitIndex}/${local.commitIndex}`);
      }

      if (follower.state.state !== "up-to-date") {
        return Error(`Replication to ${pid} not yet up-to-date`);
      }
    }

    return true;
  };
};

const agencyJobIn = function (jobId, where) {
  return function () {
    const current = LH.getAgencyJobStatus(jobId);
    if (current !== where) {
      return Error(`Job ${jobId} expected to be in ${where}, but is in ${current}`);
    }
    return true;
  };
};

const allServicesOperational = function (database, logId) {
  return function () {
    const {plan, current} = LH.readReplicatedLogAgency(database, logId);

    if (current === undefined || current.localStatus === undefined) {
      return Error("status in current not yet defined");
    }
    if (plan === undefined || plan.participantsConfig === undefined) {
      return Error("participantsConfig in plan not yet defined");
    }

    for (const [pid, _] of Object.entries(plan.participantsConfig.participants)) {
      if (pid in current.localStatus && current.localStatus[pid].state !== "ServiceOperational") {
        return Error(`Participant ${pid} not yet operational`);
      }
    }
    return true;
  };
};

const lowestIndexToKeepReached = function (log, leader, ltik) {
  return function () {
    log.ping();
    const status = log.status();
    if (status.participants[leader].response.lowestIndexToKeep >= ltik) {
      return true;
    }
    return Error(`lowestIndexToKeep should be >= ${ltik} for ${leader}, status: ${JSON.stringify(status)} `
      + `log contents ${JSON.stringify(log.head(1000))}`);
  };
};

const allFollowersAppliedEverything = function (log) {
  return function () {
    // incomplete type description, just enough for this function:
    /** @type {
     *   {
     *     participants: {
     *       response: {
     *         role: ('leader'|'follower'|'unconfigured'),
     *         local: {
     *           spearhead: {
     *             term: number,
     *             index: number
     *           },
     *           appliedIndex: number
     *         }
     *       }
     *     }
     *   }
     * }
     */
    const status = log.status();
    const participants = _(status.participants).mapValues(p => p.response);
    const leaderStatus = participants.find(p => p.role === 'leader').local;
    const spearhead = leaderStatus.spearhead.index;
    const followers = participants.pickBy(p => p.role === 'follower').mapValues(p => p.local);
    if (followers.every(p => p.appliedIndex === spearhead)) {
      return true;
    } else {
      return Error(`Followers haven't applied everything. Spearhead is ${spearhead}, and these followers have lower applied indexes: `
        + followers
            .pickBy(v => v.appliedIndex !== spearhead)
            .map((v, p) => `[${p}] ${v.appliedIndex}`)
            .join(', '));
    }
  };
};

exports.allServersHealthy = allServersHealthy;
exports.replicatedLogIsGone = replicatedLogIsGone;
exports.replicatedLogIsReady = replicatedLogIsReady;
exports.replicatedLogLeaderCommitFail = replicatedLogLeaderCommitFail;
exports.replicatedLogLeaderEstablished = replicatedLogLeaderEstablished;
exports.replicatedLogLeaderPlanIs = replicatedLogLeaderPlanIs;
exports.replicatedLogLeaderTargetIs = replicatedLogLeaderTargetIs;
exports.replicatedLogLeaderPlanChanged = replicatedLogLeaderPlanChanged;
exports.replicatedLogParticipantGeneration = replicatedLogParticipantGeneration;
exports.replicatedLogParticipantsFlag = replicatedLogParticipantsFlag;
exports.replicatedLogTargetVersion = replicatedLogTargetVersion;
exports.replicatedLogReplicationCompleted = replicatedLogReplicationCompleted;
exports.serverFailed = serverFailed;
exports.serverHealthy = serverHealthy;
exports.agencyJobIn = agencyJobIn;
exports.replicatedStateAccessible = replicatedStateAccessible;
exports.allReplicatedStatesAccessible = allReplicatedStatesAccessible;
exports.allServicesOperational = allServicesOperational;
exports.replicatedStateConverged = replicatedStateConverged;
exports.allReplicatedStatesConverged = allReplicatedStatesConverged;
exports.lowestIndexToKeepReached = lowestIndexToKeepReached;
exports.allFollowersAppliedEverything = allFollowersAppliedEverything;
