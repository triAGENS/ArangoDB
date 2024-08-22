/*jshint strict: true */
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
/// @author Lars Maier
// //////////////////////////////////////////////////////////////////////////////

const internal = require("internal");
const {wait} = internal;
const _ = require("lodash");
const request = require('@arangodb/request');
const arangodb = require('@arangodb');
const ArangoError = arangodb.ArangoError;
const ERRORS = arangodb.errors;
const db = arangodb.db;
const lpreds = require("@arangodb/testutils/replicated-logs-predicates");
const helper = require('@arangodb/test-helper-common');
const clientHelper = require('@arangodb/test-helper');
const isServer = arangodb.isServer;

const waitFor = function (checkFn, maxTries = 240, onErrorCallback) {
  const waitTimes = [0.1, 0.1, 0.2, 0.2, 0.2, 0.3, 0.5];
  const getWaitTime = (count) => waitTimes[Math.min(waitTimes.length-1, count)];
  let count = 0;
  let result = null;
  while (count < maxTries) {
    result = checkFn();
    if (result === true || result === undefined) {
      return result;
    }
    if (!(result instanceof Error)) {
      throw Error("expected error");
    }
    count += 1;
    if (count % 10 === 0) {
      console.log(result);
    }
    wait(getWaitTime(count));
  }
  if (onErrorCallback !== undefined) {
    onErrorCallback(result);
  } else {
    throw result;
  }
};

const readAgencyValueAt = function (key, jwtBearerToken) {
  const response = clientHelper.agency.get(key, jwtBearerToken);
  const path = ['arango', ...key.split('/').filter(i => i)];
  let result = response;
  for (const p of path) {
    if (result === undefined) {
      return undefined;
    }
    result = result[p];
  }
  return result;
};

const getServerRebootId = function (serverId) {
  return readAgencyValueAt(`Current/ServersKnown/${serverId}/rebootId`);
};

const isDBServerInCurrent = function (serverId) {
  return readAgencyValueAt(`Current/DBServers/${serverId}`);
};

const bumpServerRebootId = function (serverId) {
  const response = clientHelper.agency.increaseVersion(`Current/ServersKnown/${serverId}/rebootId`);
  if (response !== true) {
    return undefined;
  }
  return getServerRebootId(serverId);
};

const getParticipantsObjectForServers = function (servers) {
  return _.reduce(servers, (a, v) => {
    a[v] = {allowedInQuorum: true, allowedAsLeader: true, forced: false};
    return a;
  }, {});
};

const createParticipantsConfig = function (generation, config, servers) {
  return {
    generation,
    config,
    participants: getParticipantsObjectForServers(servers),
  };
};

const createTermSpecification = function (term, servers, leader) {
  let spec = {term};
  if (leader !== undefined) {
    if (!_.includes(servers, leader)) {
      throw Error("leader is not part of the participants");
    }
    spec.leader = {serverId: leader, rebootId: getServerRebootId(leader)};
  }
  return spec;
};

const getServerHealth = function (serverId) {
  return readAgencyValueAt(`Supervision/Health/${serverId}/Status`);
};

const dbservers = (function () {
  return clientHelper.getDBServers().map((x) => x.id);
}());
const coordinators = (function () {
  return clientHelper.getServersByType('coordinator').map((x) => x.id);
}());


/**
 * @param {string} database
 * @param {string} logId
 * @returns {{
 *       target: {
 *         id: number,
 *         leader?: string,
 *         participants: Object<string, {
 *           forced: boolean,
 *           allowedInQuorum: boolean,
 *           allowedAsLeader: boolean
 *         }>,
 *         config: {
 *           writeConcern: number,
 *           softWriteConern: number,
 *           replicationFactor: number,
 *           waitForSync: boolean
 *         },
 *         properties: {}
 *       },
 *       plan: {
 *         id: number,
 *         participantsConfig: {
 *           participants: Object<string, {
 *             forced: boolean,
 *             allowedInQuorum: boolean,
 *             allowedAsLeader: boolean
 *           }>,
 *           generation: number,
 *           config: {
 *             waitForSync: boolean,
 *             effectiveWriteConcern: number
 *           }
 *         },
 *         currentTerm?: {
 *           term: number,
 *           leader?: {
 *             serverId: string,
 *             rebootId: number
 *           }
 *         }
 *       },
 *       current: {
 *         localStatus: Object<string, Object>,
 *         localState: Object,
 *         supervision?: Object,
 *         leader?: Object,
 *         safeRebootIds?: Object<string, number>
 *       }
 *   }}
 */
const readReplicatedLogAgency = function (database, logId) {
  let target = readAgencyValueAt(`Target/ReplicatedLogs/${database}/${logId}`);
  let plan = readAgencyValueAt(`Plan/ReplicatedLogs/${database}/${logId}`);
  let current = readAgencyValueAt(`Current/ReplicatedLogs/${database}/${logId}`);
  return {target, plan, current};
};

const replicatedLogSetPlanParticipantsConfig = function (database, logId, participantsConfig) {
  clientHelper.agency.set(`Plan/ReplicatedLogs/${database}/${logId}/participantsConfig`, participantsConfig);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const replicatedLogSetTargetParticipantsConfig = function (database, logId, participantsConfig) {
  clientHelper.agency.set(`Target/ReplicatedLogs/${database}/${logId}/participants`, participantsConfig);
  clientHelper.agency.increaseVersion(`Target/Version`);
};

const replicatedLogUpdatePlanParticipantsConfigParticipants = function (database, logId, participants) {
  const oldValue = readAgencyValueAt(`Plan/ReplicatedLogs/${database}/${logId}/participantsConfig`);
  const newValue = oldValue || {generation: 0, participants: {}};
  for (const [p, v] of Object.entries(participants)) {
    if (v === null) {
      delete newValue.participants[p];
    } else {
      newValue.participants[p] = v;
    }
  }
  const gen = newValue.generation += 1;
  replicatedLogSetPlanParticipantsConfig(database, logId, newValue);
  return gen;
};

const replicatedLogUpdateTargetParticipants = function (database, logId, participants) {
  const oldValue = readAgencyValueAt(`Target/ReplicatedLogs/${database}/${logId}/participants`);
  const newValue = oldValue || {};
  for (const [p, v] of Object.entries(participants)) {
    if (v === null) {
      delete newValue[p];
    } else {
      newValue[p] = v;
    }
  }
  replicatedLogSetTargetParticipantsConfig(database, logId, newValue);
};

const replicatedLogSetPlanTerm = function (database, logId, term) {
  clientHelper.agency.set(`Plan/ReplicatedLogs/${database}/${logId}/currentTerm/term`, term);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const triggerLeaderElection = function (database, logId) {
  // This operation has to be in one envelope. Otherwise we violate the assumption
  // that they are only modified as a unit.
  clientHelper.agency.transact([[{
    [`/arango/Plan/ReplicatedLogs/${database}/${logId}/currentTerm/term`]: {
      'op': 'increment',
    },
    [`/arango/Plan/ReplicatedLogs/${database}/${logId}/currentTerm/leader`]: {
      'op': 'delete'
    }
  }]]);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

// Note that this skips the election and is only OK to use when the leader has
// persisted all log entries, as it is not re-elected.
const bumpTermOnly = function (database, logId) {
  clientHelper.agency.transact([[{
    [`/arango/Plan/ReplicatedLogs/${database}/${logId}/currentTerm/term`]: {
      'op': 'increment',
    },
  }]]);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const replicatedLogSetPlanTermConfig = function (database, logId, term) {
  clientHelper.agency.set(`Plan/ReplicatedLogs/${database}/${logId}/currentTerm`, term);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const replicatedLogSetPlan = function (database, logId, spec) {
  clientHelper.agency.set(`Plan/ReplicatedLogs/${database}/${logId}`, spec);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const replicatedLogSetTarget = function (database, logId, spec) {
  clientHelper.agency.set(`Target/ReplicatedLogs/${database}/${logId}`, spec);
  clientHelper.agency.increaseVersion(`Target/Version`);
};

const replicatedLogDeletePlan = function (database, logId) {
  clientHelper.agency.remove(`Plan/ReplicatedLogs/${database}/${logId}`);
  clientHelper.agency.increaseVersion(`Plan/Version`);
};

const replicatedLogDeleteTarget = function (database, logId) {
  clientHelper.agency.remove(`Target/ReplicatedLogs/${database}/${logId}`);
  clientHelper.agency.increaseVersion(`Target/Version`);
};

const createReconfigureJob = function (database, logId, ops) {
  const jobId = nextUniqueLogId();
  clientHelper.agency.set(`Target/ToDo/${jobId}`, {
    type: "reconfigureReplicatedLog",
    database, logId, jobId: "" + jobId,
    operations: ops,
  });
  return jobId;
};

const waitForReplicatedLogAvailable = function (id) {
  while (true) {
    try {
      let status = db._replicatedLog(id).status();
      const leaderId = status.leaderId;
      if (leaderId !== undefined && status.participants !== undefined &&
          status.participants[leaderId].connection.errorCode === 0 && status.participants[leaderId].response.role === "leader") {
        break;
      }
      console.info("replicated log not yet available");
    } catch (err) {
      const errors = [
        ERRORS.ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED.code,
        ERRORS.ERROR_REPLICATION_REPLICATED_LOG_NOT_FOUND.code
      ];
      if (errors.indexOf(err.errorNum) === -1) {
        throw err;
      }
    }

    internal.sleep(1);
  }
};


const getServerProcessID = function (serverId) {
  let arangods = [];
  try {
    arangods = global.instanceManager.arangods;
  } catch(_) {
    arangods = helper.getServersByType("dbserver");
  }
  let pos = _.findIndex(arangods,
      x => x.id === serverId);
  return arangods[pos].pid;
};

const stopServerImpl = function (serverId) {
  console.log(`suspending server ${serverId}`);
  let result = require('internal').suspendExternal(getServerProcessID(serverId));
  if (!result) {
    throw Error("Failed to suspend server");
  }
};

const continueServerImpl = function (serverId) {
  console.log(`continuing server ${serverId}`);
  let result = require('internal').continueExternal(getServerProcessID(serverId));
  if (!result) {
    throw Error("Failed to continue server");
  }
};

const continueServerWaitOk = function (serverId) {
  continueServerImpl(serverId);
  waitFor(lpreds.serverHealthy(serverId));
};

const stopServerWaitFailed = function (serverId) {
  stopServerImpl(serverId);
  waitFor(lpreds.serverFailed(serverId));
};

const nextUniqueLogId = function () {
  return parseInt(clientHelper.uniqid());
};

const registerAgencyTestBegin = function (testName) {
  clientHelper.agency.set(`Testing/${testName}/Begin`, (new Date()).toISOString());
};

const registerAgencyTestEnd = function (testName) {
  clientHelper.agency.set(`Testing/${testName}/End`, (new Date()).toISOString());
};

const getServerUrl = helper.getUrlById;

const checkRequestResult = function (requestResult, expectingError=false) {
  if (requestResult === undefined) {
    throw new ArangoError({
      'error': true,
      'code': 500,
      'errorNum': arangodb.ERROR_INTERNAL,
      'errorMessage': 'Unknown error. Request result is empty'
    });
  }

  if (requestResult.hasOwnProperty('error')) {
    if (requestResult.error) {
      if (requestResult.errorNum === arangodb.ERROR_TYPE_ERROR) {
        throw new TypeError(requestResult.errorMessage);
      }

      const error = new ArangoError(requestResult);
      error.message = requestResult.message;
      throw error;
    }

    // remove the property from the original object
    delete requestResult.error;
  }

  if (requestResult.json === undefined) {
    throw new ArangoError({
      'error': true,
      'code': 4,
      'errorNum': arangodb.ERROR_INTERNAL,
      'errorMessage': JSON.stringify(requestResult)
    });
  }

  if (requestResult.json.error && !expectingError) {
    throw new ArangoError({
      'error': true,
      'code': requestResult.json.code,
      'errorNum': requestResult.json.errorNum,
      'errorMessage': requestResult.json.errorMessage,
    });
  }

  return requestResult;
};

const getReplicatedLogLeaderPlan = function (database, logId, nothrow = false) {
  let {plan} = readReplicatedLogAgency(database, logId);
  if (!plan.currentTerm) {
    const error = Error("no current term in plan");
    if (nothrow) {
      return error;
    }
    throw error;
  }
  if (!plan.currentTerm.leader) {
    const error = Error("current term has no leader");
    if (nothrow) {
      return error;
    }
    throw error;
  }
  const leader = plan.currentTerm.leader.serverId;
  const rebootId = plan.currentTerm.leader.rebootId;
  const term = plan.currentTerm.term;
  return {leader, term, rebootId};
};

const getReplicatedLogLeaderTarget = function (database, logId) {
  let {target} = readReplicatedLogAgency(database, logId);
  return target.leader;
};

const createReplicatedLogPlanOnly = function (database, targetConfig, replicationFactor) {
  const logId = nextUniqueLogId();
  const servers = _.sampleSize(dbservers, replicationFactor);
  const leader = servers[0];
  const term = 1;
  const generation = 1;
  replicatedLogSetPlan(database, logId, {
    id: logId,
    currentTerm: createTermSpecification(term, servers, leader),
    participantsConfig: createParticipantsConfig(generation, targetConfig, servers),
    properties: {implementation: {type: "black-hole", parameters: {}}}
  });

  // wait for all servers to have reported in current
  waitFor(lpreds.replicatedLogIsReady(database, logId, term, servers, leader));
  const followers = _.difference(servers, [leader]);
  const remaining = _.difference(dbservers, servers);
  return {logId, servers, leader, term, followers, remaining};
};

const createReplicatedLogInTarget = function (database, targetConfig, replicationFactor, servers) {
  const logId = nextUniqueLogId();
  if (replicationFactor === undefined) {
    replicationFactor = 3;
  }
  if (servers === undefined) {
    servers = _.sampleSize(dbservers, replicationFactor);
  }
  replicatedLogSetTarget(database, logId, {
    id: logId,
    config: targetConfig,
    participants: getParticipantsObjectForServers(servers),
    supervision: {maxActionsTraceLength: 20},
    properties: {implementation: {type: "black-hole", parameters: {}}}
  });

  waitFor(() => {
    let {plan, current} = readReplicatedLogAgency(database, logId.toString());
    if (current === undefined) {
      return Error("current not yet defined");
    }
    if (plan === undefined) {
      return Error("plan not yet defined");
    }

    if (!plan.currentTerm) {
      return Error(`No term in plan`);
    }

    if (!current.leader) {
      return Error("Leader has not yet established its term");
    }
    if (!current.leader.leadershipEstablished) {
      return Error("Leader has not yet established its leadership");
    }

    return true;
  });

  const {leader, term} = getReplicatedLogLeaderPlan(database, logId);
  const followers = _.difference(servers, [leader]);
  return {logId, servers, leader, term, followers};
};

const createReplicatedLog = function (database, targetConfig, replicationFactor) {
  const logId = nextUniqueLogId();
  if (replicationFactor === undefined) {
    replicationFactor = 3;
  }
  const servers = _.sampleSize(dbservers, replicationFactor);
  replicatedLogSetTarget(database, logId, {
    id: logId,
    config: targetConfig,
    participants: getParticipantsObjectForServers(servers),
    supervision: {maxActionsTraceLength: 20},
    properties: {implementation: {type: "black-hole", parameters: {}}}
  });

  waitFor(lpreds.replicatedLogLeaderEstablished(database, logId, undefined, servers));

  const {leader, term} = getReplicatedLogLeaderPlan(database, logId);
  const followers = _.difference(servers, [leader]);
  return {logId, servers, leader, term, followers};
};

const createReplicatedLogWithState = function (database, targetConfig, stateType, replicationFactor) {
  const logId = nextUniqueLogId();
  if (replicationFactor === undefined) {
    replicationFactor = 3;
  }
  const servers = _.sampleSize(dbservers, replicationFactor);
  replicatedLogSetTarget(database, logId, {
    id: logId,
    config: targetConfig,
    participants: getParticipantsObjectForServers(servers),
    supervision: {maxActionsTraceLength: 20},
    properties: {implementation: {type: stateType, parameters: {}}}
  });

  waitFor(lpreds.replicatedLogLeaderEstablished(database, logId, undefined, servers));

  const {leader, term} = getReplicatedLogLeaderPlan(database, logId);
  const followers = _.difference(servers, [leader]);
  return {logId, servers, leader, term, followers};
};


const testHelperFunctions = function (database, databaseOptions = {}) {
  let previousDatabase, databaseExisted = true;
  let stoppedServers = {};

  const stopServer = function (serverId) {
    if (stoppedServers[serverId] !== undefined) {
      throw new Error(`{serverId} already stopped`);
    }
    stopServerImpl(serverId);
    stoppedServers[serverId] = true;
  };

  const stopServerWait = function (serverId) {
    stopServersWait([serverId]);
  };

  const stopServersWait = function (serverIds) {
    serverIds.forEach(stopServer);
    serverIds.forEach(serverId => waitFor(lpreds.serverFailed(serverId)));
  };

  const continueServer = function (serverId) {
    if (stoppedServers[serverId] === undefined) {
      throw new Error(`{serverId} not stopped`);
    }
    continueServerImpl(serverId);
    delete stoppedServers[serverId];
  };

  const continueServerWait = function (serverId) {
    continueServersWait([serverId]);
  };

  const continueServersWait = function (serverIds) {
    serverIds.forEach(continueServer);
    serverIds.forEach(serverId => waitFor(lpreds.serverHealthy(serverId)));
  };

  const resumeAll = function () {
    continueServersWait(Object.keys(stoppedServers));
    stoppedServers = {};
  };

  const createTestDatabase = function () {
    previousDatabase = db._name();
    if (!_.includes(db._databases(), database)) {
      db._createDatabase(database, databaseOptions);
      databaseExisted = false;
    }
    db._useDatabase(database);
  };

  const resetPreviousDatabase = function () {
    db._useDatabase(previousDatabase);
    if (!databaseExisted) {
      db._dropDatabase(database);
    }
  };

  const setUpAll = function () {
    createTestDatabase();
  };

  const tearDownAll = function () {
    resumeAll();
    resetPreviousDatabase();
  };

  const setUpAnd = function (cb) {
    return function (testName) {
      registerAgencyTestBegin(testName);
      cb(testName);
    };
  };
  const setUp = setUpAnd(() => null);

  const tearDownAnd = function (cb) {
    return function (testName) {
      registerAgencyTestEnd(testName);
      resumeAll();
      cb(testName);
    };
  };
  const tearDown = tearDownAnd(() => null);

  return {
    stopServer,
    stopServerWait,
    stopServersWait,
    continueServer,
    continueServerWait,
    continueServersWait,
    resumeAll,
    setUpAll,
    tearDownAll,
    setUp,
    setUpAnd,
    tearDown,
    tearDownAnd
  };
};

const getSupervisionActions = function (database, logId) {
  const {current} = readReplicatedLogAgency(database, logId);
  // filter out all empty actions
  return _.filter(current.actions, (a) => a.desc.type !== "EmptyAction");
};

const getSupervisionActionTypes = function (database, logId) {
  const actions = getSupervisionActions(database, logId);
  return _.map(actions, (a) => a.desc.type);
};

const getLastNSupervisionActionsType = function (database, logId, n) {
  const actions = getSupervisionActionTypes(database, logId);
  return _.takeRight(actions, n);
};

const countActionsByType = function (actions) {
  return _.reduce(actions, function (acc, type) {
    acc[type] = 1 + (acc[type] || 0);
    return acc;
  }, {});
};

const updateReplicatedLogTarget = function(database, id, callback) {
  const {target: oldTarget} = readReplicatedLogAgency(database, id);
  let result = callback(oldTarget);
  if (result === undefined) {
    result = oldTarget;
  }
  replicatedLogSetTarget(database, id, result);
};

const increaseTargetVersion = function (database, id) {
  const {target} = readReplicatedLogAgency(database, id);
  if (target) {
    target.version = 1 + (target.version || 0);
    replicatedLogSetTarget(database, id, target);
    return target.version;
  }
};

const sortedArrayEqualOrError = (left, right) => {
  if (_.isEqual(left, right)) {
    return true;
  } else {
    return Error(`Expected the following to be equal: ${JSON.stringify(left)} and ${JSON.stringify(right)}`);
  }
};

const shardIdToLogId = function (shardId) {
  return shardId.slice(1);
};

const dumpLogHead = function (logId, limit=1000) {
  let log = db._replicatedLog(logId);
  return log.head(limit);
};

const getShardsToLogsMapping = function (dbName, colId, jwtBearerToken) {
  const colPlan = readAgencyValueAt(`Plan/Collections/${dbName}/${colId}`, jwtBearerToken);
  let mapping = {};
  if (colPlan.hasOwnProperty("groupId")) {
    const groupId = colPlan.groupId;
    const shards = colPlan.shardsR2;
    const colGroup = readAgencyValueAt(`Plan/CollectionGroups/${dbName}/${groupId}`, jwtBearerToken);
    const shardSheaves = colGroup.shardSheaves;
    for (let idx = 0; idx < shards.length; ++idx) {
      mapping[shards[idx]] = shardSheaves[idx].replicatedLog;
    }
  } else {
    // Legacy code, supporting system collections
    const shards = colPlan.shards;
    for (const [shardId, _] of Object.entries(shards)) {
      mapping[shardId] = shardIdToLogId(shardId);
    }
  }
  return mapping;
};

const setLeader = (database, logId, newLeader) => {
  const url = getServerUrl(_.sample(coordinators));
  const res = request.post(`${url}/_db/${database}/_api/log/${logId}/leader/${newLeader}`);
  checkRequestResult(res);
  const { json: { result } } = res;
  return result;
};

const unsetLeader = (database, logId) => {
  const url = getServerUrl(_.sample(coordinators));
  const res = request.delete(`${url}/_db/${database}/_api/log/${logId}/leader`);
  checkRequestResult(res);
  const { json: { result } } = res;
  return result;
};

/**
 * Causes underlying replicated logs to trigger leader recovery.
 */
const bumpTermOfLogsAndWaitForConfirmation = function (dbn, col) {
  const {numberOfShards, isSmart} = col.properties();
  if (isSmart && numberOfShards === 0) {
    // Adjust for SmartEdgeCollections
    col = db._collection(`_local_${col.name()}`);
  }
  const shards = col.shards();
  const shardsToLogs = getShardsToLogsMapping(dbn, col._id);
  const stateMachineIds = shards.map(s => shardsToLogs[s]);

  // Clear the old leader, so it doesn't get back automatically.
  stateMachineIds.forEach((stateId) => unsetLeader(dbn, stateId));
  stateMachineIds.forEach((stateId) => triggerLeaderElection(dbn, stateId));

  const leaderReady = (stateId) => lpreds.replicatedLogLeaderEstablished(dbn, stateId, undefined, []);

  stateMachineIds.forEach(x => waitFor(leaderReady(x)));
};

const getLocalStatus = function (serverId, database, logId) {
  let url = getServerUrl(serverId);
  const res = request.get(`${url}/_db/${database}/_api/log/${logId}/local-status`);
  checkRequestResult(res);
  return res.json.result;
};

const replaceParticipant = (database, logId, oldParticipant, newParticipant) => {
  const url = getServerUrl(_.sample(coordinators));
  const res = request.post(
      `${url}/_db/${database}/_api/log/${logId}/participant/${oldParticipant}/replace-with/${newParticipant}`
  );
  checkRequestResult(res);
  const {json: {result}} = res;
  return result;
};

const getAgencyJobStatus = function (jobId) {
  const places = ["ToDo", "Pending", "Failed", "Finished"];
  for (const p of places) {
    const job = readAgencyValueAt(`Target/${p}/${jobId}`);
    if (job !== undefined) {
      return p;
    }
  }
  return "NotFound";
};

exports.checkRequestResult = checkRequestResult;
exports.continueServer = continueServerImpl;
exports.continueServerWaitOk = continueServerWaitOk;
exports.coordinators = coordinators;
exports.countActionsByType = countActionsByType;
exports.createParticipantsConfig = createParticipantsConfig;
exports.createReplicatedLog = createReplicatedLog;
exports.createReplicatedLogPlanOnly = createReplicatedLogPlanOnly;
exports.createTermSpecification = createTermSpecification;
exports.dbservers = dbservers;
exports.getLastNSupervisionActionsType = getLastNSupervisionActionsType;
exports.getLocalStatus = getLocalStatus;
exports.getParticipantsObjectForServers = getParticipantsObjectForServers;
exports.getReplicatedLogLeaderPlan = getReplicatedLogLeaderPlan;
exports.getReplicatedLogLeaderTarget = getReplicatedLogLeaderTarget;
exports.getServerHealth = getServerHealth;
exports.getServerRebootId = getServerRebootId;
exports.isDBServerInCurrent = isDBServerInCurrent;
exports.bumpServerRebootId = bumpServerRebootId;
exports.getServerUrl = getServerUrl;
exports.getSupervisionActionTypes = getSupervisionActionTypes;
exports.getSupervisionActions = getSupervisionActions;
exports.nextUniqueLogId = nextUniqueLogId;
exports.readAgencyValueAt = readAgencyValueAt;
exports.readReplicatedLogAgency = readReplicatedLogAgency;
exports.registerAgencyTestBegin = registerAgencyTestBegin;
exports.registerAgencyTestEnd = registerAgencyTestEnd;
exports.replicatedLogDeletePlan = replicatedLogDeletePlan;
exports.replicatedLogDeleteTarget = replicatedLogDeleteTarget;
exports.replicatedLogSetPlan = replicatedLogSetPlan;
exports.replicatedLogSetPlanParticipantsConfig = replicatedLogSetPlanParticipantsConfig;
exports.replicatedLogSetPlanTermConfig = replicatedLogSetPlanTermConfig;
exports.replicatedLogSetPlanTerm = replicatedLogSetPlanTerm;
exports.replicatedLogSetTarget = replicatedLogSetTarget;
exports.replicatedLogUpdatePlanParticipantsConfigParticipants = replicatedLogUpdatePlanParticipantsConfigParticipants;
exports.replicatedLogUpdateTargetParticipants = replicatedLogUpdateTargetParticipants;
exports.stopServer = stopServerImpl;
exports.stopServerWaitFailed = stopServerWaitFailed;
exports.testHelperFunctions = testHelperFunctions;
exports.updateReplicatedLogTarget = updateReplicatedLogTarget;
exports.waitFor = waitFor;
exports.waitForReplicatedLogAvailable = waitForReplicatedLogAvailable;
exports.sortedArrayEqualOrError = sortedArrayEqualOrError;
exports.shardIdToLogId = shardIdToLogId;
exports.dumpLogHead = dumpLogHead;
exports.setLeader = setLeader;
exports.unsetLeader = unsetLeader;
exports.createReplicatedLogWithState = createReplicatedLogWithState;
exports.bumpTermOfLogsAndWaitForConfirmation = bumpTermOfLogsAndWaitForConfirmation;
exports.bumpTermOnly = bumpTermOnly;
exports.getShardsToLogsMapping = getShardsToLogsMapping;
exports.replaceParticipant = replaceParticipant;
exports.createReconfigureJob = createReconfigureJob;
exports.getAgencyJobStatus = getAgencyJobStatus;
exports.increaseTargetVersion = increaseTargetVersion;
exports.triggerLeaderElection = triggerLeaderElection;
exports.createReplicatedLogInTarget = createReplicatedLogInTarget;
