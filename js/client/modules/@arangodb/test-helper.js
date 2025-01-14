/*jshint strict: false */
/* global print */
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
// / @author Wilfried Goesgens
// / @author Copyright 2011-2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const internal = require('internal'); // OK: processCsvFile
const {
  runWithRetry,
  helper,
  deriveTestSuite,
  deriveTestSuiteWithnamespace,
  typeName,
  isEqual,
  compareStringIds,
  endpointToURL,
  versionHas,
  isEnterprise,
} = require('@arangodb/test-helper-common');
const fs = require('fs');
const _ = require('lodash');
const inst = require('@arangodb/testutils/instance');
const im = require('@arangodb/testutils/instance-manager');
const request = require('@arangodb/request');
const arangosh = require('@arangodb/arangosh');
const pu = require('@arangodb/testutils/process-utils');
const jsunity = require('jsunity');
const { isCluster } = require('../../bootstrap/modules/internal');
const arango = internal.arango;
const db = internal.db;
const {assertTrue, assertFalse, assertEqual} = jsunity.jsUnity.assertions;

exports.runWithRetry = runWithRetry;
exports.isEnterprise = isEnterprise;
exports.versionHas = versionHas;
exports.helper = helper;
exports.deriveTestSuite = deriveTestSuite;
exports.deriveTestSuiteWithnamespace = deriveTestSuiteWithnamespace;
exports.typeName = typeName;
exports.isEqual = isEqual;
exports.compareStringIds = compareStringIds;

let instanceManager = null;
const tmpDirMngr = require('@arangodb/testutils/tmpDirManager').tmpDirManager;
const {sanHandler} = require('@arangodb/testutils/san-file-handler');

exports.flushInstanceInfo = () => {
  instanceManager = null;
};

exports.getInstanceInfo = function() {
  if (global.hasOwnProperty('instanceManager')) {
    return global.instanceManager;
  }
  if (instanceManager === null) {
    instanceManager = new im.instanceManager('tcp', {dummy: true}, "", "/tmp/");
    instanceManager.setFromStructure(JSON.parse(internal.env.INSTANCEINFO));
  }
  return instanceManager;
};

let reconnectRetry = exports.reconnectRetry = require('@arangodb/replication-common').reconnectRetry;

exports.clearAllFailurePoints = function () {
  const old = db._name();
  try {
    for (const server of exports.getDBServers()) {
      exports.debugClearFailAt(exports.getEndpointById(server.id));
    }
    for (const server of exports.getCoordinators()) {
      exports.debugClearFailAt(exports.getEndpointById(server.id));
    }
  } finally {
    // need to restore original database, as debugFailAt() can 
    // change into a different database...
    db._useDatabase(old);
  }
};

/// @brief set failure point
exports.debugCanUseFailAt = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    
    let res = arango.GET_RAW('/_admin/debug/failat');
    return res.code === 200;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

/// @brief set failure point
exports.debugSetFailAt = function (endpoint, failAt) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.PUT_RAW('/_admin/debug/failat/' + failAt, {});
    if (res.parsedBody !== true) {
      throw `Error setting failure point ${failAt} on ${endpoint}: "${JSON.stringify(res)}"`;
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

exports.debugResetRaceControl = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/raceControl');
    if (res.code !== 200) {
      throw "Error resetting race control.";
    }
    return false;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

/// @brief remove failure point
exports.debugRemoveFailAt = function (endpoint, failAt) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/failat/' + failAt);
    if (res.code !== 200) {
      throw "Error removing failure point";
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

exports.debugClearFailAt = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.DELETE_RAW('/_admin/debug/failat');
    if (res.code !== 200) {
      throw "Error removing failure points";
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

exports.getChecksum = function (endpoint, name) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.GET_RAW('/_api/collection/' + name + '/checksum');
    if (res.code !== 200) {
      throw "Error getting collection checksum";
    }
    return res.parsedBody.checksum;
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
  }
};

exports.getRawMetric = function (endpoint, tags) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    if (endpoint !== primaryEndpoint) {
      reconnectRetry(endpoint, db._name(), "root", "");
    }
    return arango.GET_RAW('/_admin/metrics' + tags, { 'accept-encoding': 'identity' });
  } finally {
    if (endpoint !== primaryEndpoint) {
      reconnectRetry(primaryEndpoint, db._name(), "root", "");
    }
  }
};

exports.getAllMetric = function (endpoint, tags) {
  let res = exports.getRawMetric(endpoint, tags);
  if (res.code !== 200) {
    throw "error fetching metric";
  }
  return res.body;
};

function getMetricName(text, name) {
  let re = new RegExp("^" + name);
  let matches = text.split('\n').filter((line) => !line.match(/^#/)).filter((line) => line.match(re));
  if (!matches.length) {
    throw "Metric " + name + " not found";
  }
  let res = 0; // Sum up values from all matches
  for(let i = 0; i < matches.length; i+= 1) {
    res += Number(matches[i].replace(/^.*?(\{.*?\})?\s*([0-9.]+)$/, "$2"));
  }
  return res;
}

exports.getMetric = function (endpoint, name) {
  let text = exports.getAllMetric(endpoint, '');
  return getMetricName(text, name);
};

exports.getMetricSingle = function (name) {
  let res = arango.GET_RAW("/_admin/metrics");
  if (res.code !== 200) {
    throw "error fetching metric";
  }
  return getMetricName(res.body, name);
};

// Function for getting metric/metrics from either cluster or single server deployments.
// - 'name' - can be either string or array of strings.
//    If 'name' is string, we want to get the only one metric value with name 'name'
//    If 'name' is array of strings, we want to get values for every metric which is defined in this array 
// - 'roles' - string
//    Specify which roles of arangod should be queried for particular metric/metrics.
//    This argument will be used in function getAllMetricsFromEndpoints.
//    Possible values are:
//      "coordinators" - get metric/metrics only from coordinators.
//      "dbservers" - get metric/metrics only from dbservers.
//      "all" - get metric/metrics from dbservers and from coordinators.
//    In case of single server deployment, this argument is ommited.
exports.getCompleteMetricsValues = function (name, roles = "") {
  function transpose(matrix) {
    return matrix[0].map((col, i) => matrix.map(row => row[i]));
  };

  let metrics = exports.getMetricsByNameFromEndpoints(name, roles);

  if (typeof name === "string") {
    // In case of "string", 'metrics' variable is an array with values of metric from each server

    // It may happen that some db servers will not have required metric. 
    // But if all of them don't have it - throw a error.
    if (metrics.every( (val) => Number.isNaN(val) === true )) {
      // If we have got NaN from every endpoint - throw error
      throw "Metric " + name + " not found";
    }
    let res = 0;
    metrics.forEach( num => {
      if(!Number.isNaN(num)) {
        // avoid adding NaN
        res += num;
      }
    });
    assertFalse(Number.isNaN(res)); // res should not be NaN
    return res;
  } else {
    let result_metrics = [];
    // 'metrics' variable is a matrix. Every row represent metrics values from only one dbserver.
    // Number of rows equal to number of dbservers
    // Number of columns equal to number of required metrics (name.length)
    // We will transpose this matrix. After that in row 'i' we have values for metric 'i' from all servers.
    let m = transpose(metrics);

    for (let i = 0; i < m.length; i += 1) {
      let row = m[i]; // Every row represents values for this metric from all servers.
      // Now assert that at least one dbserver returned real value. Throw exception otherwise
      assertFalse(row.every((val) => Number.isNaN(val) === true), `Metric ${name[i]} not found`);
      
      let accumulated = 0;
      row.forEach(v => {
        if (!Number.isNaN(v)) {
          accumulated += v;
        }
      });
      result_metrics.push(accumulated);
    }
    // Result array shouldn't contain NaN at all
    assertTrue(result_metrics.every((val) => Number.isNaN(val) === false));
    return result_metrics;
  }
};

const debug = function (text) {
  console.warn(text);
};

const runShell = function(args, prefix, sanHnd) {
  let options = internal.options();

  let endpoint = arango.getEndpoint().replace(/\+vpp/, '').replace(/^http:/, 'tcp:').replace(/^https:/, 'ssl:').replace(/^vst:/, 'tcp:').replace(/^h2:/, 'tcp:');
  let moreArgs = {
    'javascript.startup-directory': options['javascript.startup-directory'],
    'server.endpoint': endpoint,
    'server.database': arango.getDatabaseName(),
    'server.username': arango.connectedUser(),
    'server.password': '',
    'log.foreground-tty': 'false',
    'log.output': 'file://' + prefix + '.log'
  };
  _.assign(args, moreArgs);
  let argv = internal.toArgv(args);

  for (let o in options['javascript.module-directory']) {
    argv.push('--javascript.module-directory');
    argv.push(options['javascript.module-directory'][o]);
  }

  let result = internal.executeExternal(global.ARANGOSH_BIN, argv, false /*usePipes*/, sanHnd.getSanOptions());
  assertTrue(result.hasOwnProperty('pid'));
  let status = internal.statusExternal(result.pid);
  assertEqual(status.status, "RUNNING");
  return result.pid;
};

const buildCode = function(dbname, key, command, cn, duration) {
  
  let file = fs.getTempFile() + "-" + key;
  fs.write(file, `
(function() {
// For chaos tests additional 10 secs might be not enough, so add 3 minutes buffer
require('internal').SetGlobalExecutionDeadlineTo((${duration} + 180) * 1000);
let tries = 0;
while (true) {
  ++tries;
  try {
    if (db['${cn}'].exists('stop')) {
      break;
    }
  } catch (err) {
    // the operation may actually fail because of failure points
  }
  ${command}
}
let saveTries = 0;
while (++saveTries < 100) {
  try {
    /* saving our status may actually fail because of failure points set */
    db['${cn}'].insert({ _key: "${key}", done: true, iterations: tries });
    break;
  } catch (err) {
    /* try again */
  }
}
})();
  `);

  let sanHnd = new sanHandler(pu.ARANGOSH_BIN, global.instanceManager.options);
  let tmpMgr = new tmpDirMngr(fs.join(`chaos_${key}`), global.instanceManager.options);
  
  let args = {'javascript.execute': file};
  args["--server.database"] = dbname;
  sanHnd.detectLogfiles(tmpMgr.tempDir, tmpMgr.tempDir);
  let pid = runShell(args, file, sanHnd);
  debug("started client with key '" + key + "', pid " + pid + ", args: " + JSON.stringify(args));
  return { key, file, pid,  sanHnd, tmpMgr};
};
exports.runShell = runShell;

const abortSignal = 6;

exports.runParallelArangoshTests = function (tests, duration, cn) {
  assertTrue(fs.isFile(pu.ARANGOSH_BIN), "arangosh executable not found!");
  
  assertFalse(db[cn].exists("stop"));
  let clients = [];
  debug(`starting ${tests.length} test clients`);
  try {
    tests.forEach(function (test) {
      let key = test[0];
      let code = test[1];
      let client = buildCode(db._name(), key, code, cn, duration);
      client.done = false;
      client.failed = true; // assume the worst
      clients.push(client);
    });

    debug(`running test with ${tests.length} clients for ${duration} s...`);

    let reportCounter = 0;
    for (let count = 0; count < duration; count ++) {
      internal.sleep(1);
      clients.forEach(function (client) {
        if (!client.done) {
          let status = internal.statusExternal(client.pid, false);
          if (status.status !== 'RUNNING') {
            client.done = true;
            client.failed = true;
            debug(`Client ${client.pid} exited before the duration end. Aborting tests: ${JSON.stringify(status)}`);
            count = duration + 10;
            client.sanHnd.fetchSanFileAfterExit(status.pid);
          }
        }
      });
      if (count >= duration + 10) {
        clients.forEach(function (client) {
          if (!client.done) {
            debug(`force terminating ${client.pid} since we're aborting the tests`);
            internal.killExternal(client.pid, abortSignal);
            internal.statusExternal(client.pid, false);
            client.failed = true;
          }
        });
      }

      if (++reportCounter % 15 === 0) {
        debug(`  ...${reportCounter} s into the test...`);
      }
    }

    // clear failure points
    debug("clearing all potential failure points");
    global.instanceManager.debugClearFailAt();
  
    debug("stopping all test clients");
    // broad cast stop signal
    assertFalse(db[cn].exists("stop"));
    let saveTries = 0;
    while (++saveTries < 100) {
      try {
        // saving our stop signal may actually fail because of failure points set
        db[cn].insert({ _key: "stop" }, { overwriteMode: "ignore" });
        break;
      } catch (err) {
        // try again
      }
    }

    const allClientsDone = () => clients.every(client => client.done);
    const maxTries = (versionHas('asan') || versionHas('tsan')) ? 240 : 120;
    let tries = 0;
    while (++tries < maxTries) {
      clients.forEach(function (client) {
        if (!client.done) {
          let status = internal.statusExternal(client.pid);
          if (status.status === 'NOT-FOUND' || status.status === 'TERMINATED') {
            client.done = true;
          }
          if (status.status === 'TERMINATED' && status.exit === 0) {
            client.failed = false;
          }
        }
      });

      if (allClientsDone()) {
        break;
      }

      internal.sleep(0.5);
    }

    if (!allClientsDone()) {
      console.warn("Not all shells could be joined!");
    }
  } finally {
    clients.forEach(function(client) {
      try {
        if (!client.failed) {
          fs.remove(client.file);
        }
      } catch (err) { }

      const logfile = client.file + '.log';
      if (client.failed) {
        if (fs.exists(logfile) && fs.readFileSync(logfile).toString() !== '') {
          debug(`test client with pid ${client.pid} has failed and wrote logfile '${logfile}: ${fs.readFileSync(logfile).toString()}`);
        } else {
          debug(`test client with pid ${client.pid} has failed and did not write a logfile`);
        }
      }
      try {
        if (!client.failed) {
          fs.remove(logfile);
        }
      } catch (err) { }

      if (!client.done) {
        // hard-kill all running instances
        try {
          let status = internal.statusExternal(client.pid).status;
          if (status === 'RUNNING') {
            debug(`forcefully killing test client with pid ${client.pid}`);
            internal.killExternal(client.pid, 9 /*SIGKILL*/);
          }
        } catch (err) { }
      }
    });
  }
  return clients;
};

exports.waitForEstimatorSync = function() {
  return arango.POST("/_admin/execute", "require('internal').waitForEstimatorSync();"); // make sure estimates are consistent
};

exports.waitForShardsInSync = function (cn, timeout, minimumRequiredFollowers = 0) {
  if (!timeout) {
    timeout = 300;
  }
  let start = internal.time();
  while (true) {
    if (internal.time() - start > timeout) {
      assertTrue(false, `${Date()} Shards for collection '${cn}' were not getting in sync in time, giving up!`);
    }
    let shardDistribution = arango.GET("/_admin/cluster/shardDistribution");
    assertFalse(shardDistribution.error);
    assertEqual(200, shardDistribution.code);
    let collInfo = shardDistribution.results[cn];
    let shards = Object.keys(collInfo.Plan);
    let insync = 0;
    for (let s of shards) {
      if (collInfo.Plan[s].followers.length === collInfo.Current[s].followers.length
        && minimumRequiredFollowers <= collInfo.Plan[s].followers.length) {
        ++insync;
      }
    }
    if (insync === shards.length) {
      return;
    }
    console.warn("insync=", insync, ", collInfo=", collInfo, internal.time() - start);
    internal.wait(1);
  }
};

exports.getControleableServers = function (role) {
  return global.theInstanceManager.arangods.filter((instance) => instance.isRole(role));
};

// These functions lean on special runners to export the actual instance object into the global namespace.
exports.getCtrlAgents = function() {
  return exports.getControleableServers(inst.instanceRole.agent);
};
exports.getCtrlDBServers = function() {
  return exports.getControleableServers(inst.instanceRole.dbServer);
};
exports.getCtrlCoordinators = function() {
  return exports.getControleableServers(inst.instanceRole.coordinator);
};

exports.getServers = function (role) {
  let ret = exports.getInstanceInfo().arangods.filter(arangod => arangod.isRole(role));
  if (ret.length === 0) {
    throw new Error("No instance matched the type " + role);
  }
  return ret;
};

exports.getCoordinators = function () {
  return exports.getServers(inst.instanceRole.coordinator);
};
exports.getDBServers = function () {
  return exports.getServers(inst.instanceRole.dbServer);
};
exports.getAgents = function () {
  return exports.getServers(inst.instanceRole.agent);
};

exports.getServerById = function (id) {
  const instanceInfo = exports.getInstanceInfo();
  return instanceInfo.arangods.find((d) => (d.id === id));
};

exports.getServersByType = function (type) {
  const isType = (d) => (d.instanceRole.toLowerCase() === type);
  const instanceInfo = exports.getInstanceInfo();
  return instanceInfo.arangods.filter(isType);
};

exports.getEndpointById = function (id) {
  const toEndpoint = (d) => (d.endpoint);

  const instanceInfo = exports.getInstanceInfo();
  const instance = instanceInfo.arangods.find(d => d.id === id);
  return endpointToURL(toEndpoint(instance));
};

exports.getUrlById = function (id) {
  const toUrl = (d) => (d.url);
  const instanceInfo = exports.getInstanceInfo();
  return instanceInfo.arangods.filter((d) => (d.id === id))
    .map(toUrl)[0];
};

exports.getEndpointsByType = function (type) {
  const isType = (d) => (d.instanceRole.toLowerCase() === type);
  const toEndpoint = (d) => (d.endpoint);

  const instanceInfo = exports.getInstanceInfo();
  return instanceInfo.arangods.filter(isType)
    .map(toEndpoint)
    .map(endpointToURL);
};

exports.triggerMetrics = function () {
  let coordinators = exports.getEndpointsByType("coordinator");
  exports.getRawMetric(coordinators[0], '?mode=write_global');
  for (let i = 1; i < coordinators.length; i++) {
    let c = coordinators[i];
    exports.getRawMetric(c, '?mode=trigger_global');
  }
  require("internal").sleep(2);
};

exports.activateFailure = function (name) {
  const isCluster = require("internal").isCluster();
  let roles = [];
  if (isCluster) {
    roles.push("dbserver");
    roles.push("coordinator");
  } else {
    roles.push("single");
  }
  
  roles.forEach(role => {
    exports.getEndpointsByType(role).forEach(ep => exports.debugSetFailAt(ep, name));
  });

};

exports.deactivateFailure = function (name) {
  const isCluster = require("internal").isCluster();
  let roles = [];
  if (isCluster) {
    roles.push("dbserver");
    roles.push("coordinator");
  } else {
    roles.push("single");
  }

  roles.forEach(role => {
    exports.getEndpointsByType(role).forEach(ep => exports.debugClearFailAt(ep, name));
  });
};

exports.getMaxNumberOfShards = function () {
  return arango.POST("/_admin/execute", "return require('internal').maxNumberOfShards;");
};

exports.getMaxReplicationFactor = function () {
  return arango.POST("/_admin/execute", "return require('internal').maxReplicationFactor;");
};

exports.getMinReplicationFactor = function () {
  return arango.POST("/_admin/execute", "return require('internal').minReplicationFactor;");
};

exports.getDbPath = function () {
  return arango.POST("/_admin/execute", `return require("internal").db._path();`);
};

exports.getResponsibleShardFromClusterInfo = function (vertexCollectionId, v) {
  return arango.POST("/_admin/execute", `return global.ArangoClusterInfo.getResponsibleShard(${JSON.stringify(vertexCollectionId)}, ${JSON.stringify(v)})`);
};

exports.getResponsibleServersFromClusterInfo = function (arg) {
  return arango.POST("/_admin/execute", `return global.ArangoClusterInfo.getResponsibleServers(${JSON.stringify(arg)});`);
};

exports.getAllMetricsFromEndpoints = function (roles = "") {
  const isCluster = require("internal").isCluster();
  
  let res = [];
  let endpoints = [];
  
  if (isCluster) {
    exports.triggerMetrics();

    if (roles === "" || roles === "dbservers" || roles === "all") {
      endpoints = endpoints.concat(exports.getDBServerEndpoints());
    }
    if (roles === "coordinators" || roles === "all") {
      endpoints = endpoints.concat(exports.getCoordinatorEndpoints());
    }
  } else {
    endpoints = endpoints.concat(exports.getSingleServerEndpoint());
  }

  endpoints.forEach(e => {
    res.push(exports.getAllMetric(e, ''));
  });
  return res;
};

exports.getMetricsByNameFromEndpoints = function (name, roles = "") {
  function func (text, name_str) {
    let value;
    try {
      value = getMetricName(text, name_str);
    } catch (e) {
      value = NaN;
    }
    return value;
  };
  let result = [];

  // This is an array with metrics from all required endpoints.
  let all_server_metrics = exports.getAllMetricsFromEndpoints(roles);
  // Now we need to parse every element from this array and extract
  // required metrics.
  all_server_metrics.forEach(server_metrics => {
    if (typeof name === "string") {
      result.push(func(server_metrics, name));
    } else if (typeof name === "object") {
      let res = [];
      name.forEach(curr_metric_name => {
        res.push(func(server_metrics, curr_metric_name));
      });
      result.push(res);
    } else {
      throw Error(`Unsupported ${typeof name} type`);
    }   
  });
  return result;
};

exports.getEndpoints = function (role) {
  return exports.getServers(role).map(instance => endpointToURL(instance.endpoint));
};

exports.getSingleServerEndpoint = function () {
  return exports.getEndpoints(inst.instanceRole.single);
};
exports.getCoordinatorEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.coordinator);
};
exports.getDBServerEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.dbServer);
};
exports.getAgentEndpoints = function () {
  return exports.getEndpoints(inst.instanceRole.agent);
};

const callAgency = function (operation, body, jwtBearerToken) {
  // Memoize the agents
  const getAgents = (function () {
    let agents;
    return function () {
      if (!agents) {
        agents = exports.getAgentEndpoints();
      }
      return agents;
    };
  }());
  const agents = getAgents();
  assertTrue(agents.length > 0, 'No agents present');
  const req = {
      url: `${agents[0]}/_api/agency/${operation}`,
      body: JSON.stringify(body),
      timeout: 300,
  };
  if (jwtBearerToken) {
      req.auth = { bearer: jwtBearerToken };
  }
  const res = request.post(req);
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  assertTrue(res.hasOwnProperty('json'));
  return arangosh.checkRequestResult(res.json);
};

// client-side API compatible to global.ArangoAgency
exports.agency = {
  get: function (key, jwtBearerToken) {
    const res = callAgency('read', [[
      `/arango/${key}`,
    ]], jwtBearerToken);
    return res[0];
  },

  set: function (path, value, jwtBearerToken) {
    return callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'set',
        'new': value,
      },
    }]], jwtBearerToken);
  },

  remove: function(path, jwtBearerToken) {
    return callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'delete'
      },
    }]], jwtBearerToken);
  },

  call: callAgency,

  transact: function (body, jwtBearerToken) {
    return callAgency("transact", body, jwtBearerToken);
  },

  increaseVersion: function (path, jwtBearerToken) {
    return callAgency('write', [[{
      [`/arango/${path}`]: {
        'op': 'increment',
      },
    }]], jwtBearerToken);
  },
  // TODO implement the rest...
};

exports.uniqid = function  () {
  return JSON.parse(db._connection.POST("/_admin/execute?returnAsJSON=true", "return global.ArangoClusterInfo.uniqid()"));
};

exports.arangoClusterInfoFlush = function () {
  return arango.POST("/_admin/execute", `return global.ArangoClusterInfo.flush()`);
};

exports.arangoClusterInfoGetCollectionInfo = function (dbName, collName) {
  return arango.POST("/_admin/execute", 
    `return global.ArangoClusterInfo.getCollectionInfo(${JSON.stringify(dbName)}, ${JSON.stringify(collName)})`);
};

exports.arangoClusterInfoGetCollectionInfoCurrent = function (dbName, collName, shard) {
  return arango.POST("/_admin/execute", 
    `return global.ArangoClusterInfo.getCollectionInfoCurrent(
      ${JSON.stringify(dbName)}, 
      ${JSON.stringify(collName)}, 
      ${JSON.stringify(shard)})`);
};

exports.arangoClusterInfoGetAnalyzersRevision = function (dbName) {
  return arango.POST("/_admin/execute", `return global.ArangoClusterInfo.getAnalyzersRevision(${JSON.stringify(dbName)})`);
};

exports.arangoClusterInfoWaitForPlanVersion = function (requiredVersion) {
  return arango.POST("/_admin/execute", `return global.ArangoClusterInfo.waitForPlanVersion(${JSON.stringify(requiredVersion)})`);
};

exports.AQL_EXPLAIN = function(query, bindVars, options) {
  let stmt = db._createStatement(query);
  if (typeof bindVars === "object") {
    stmt.bind(bindVars);
  }
  if (typeof options === "object") {
    stmt.setOptions(options);
  }
  return stmt.explain();
};

exports.AQL_EXECUTE = function(query, bindVars, options) {
  let cursor = db._query(query, bindVars, options);
  let extra = cursor.getExtra();
  return {
    json: cursor.toArray(),
    stats: extra.stats,
    warnings: extra.warnings,
    profile: extra.profile,
    plan: extra.plan,
    cached: cursor.cached
  };
};

exports.insertManyDocumentsIntoCollection 
  = function(db, coll, maker, limit, batchSize) {
  // This function uses the asynchronous API of `arangod` to quickly
  // insert a lot of documents into a collection. You can control which
  // documents to insert with the `maker` function. The arguments are:
  //  db - name of the database (string)
  //  coll - name of the collection, must already exist (string)
  //  maker - a callback function to produce documents, it is called
  //          with a single integer, the first time with 0, then with 1 
  //          and so on. The callback function should return an object 
  //          or a list of objects, which will be inserted into the
  //          collection. You can either specify the `_key` attribute or 
  //          not. Once you return either `null` or `false`, no more 
  //          callbacks will be done.
  //  limit - an integer, if `limit` documents have been received, no more 
  //          callbacks are issued.
  //  batchSize - an integer, this function will use this number as batch 
  //              size.
  // Example:
  //   insertManyDocumentsIntoCollection("_system", "coll",
  //       i => {Hallo:i}, 1000000, 1000);
  // will insert 1000000 documents into the collection "coll" in the 
  // `_system` database in batches of 1000. The documents will all have 
  // the `Hallo` attribute set to one of the numbers from 0 to 999999.
  // This is useful to quickly generate test data. Be careful, this can
  // create a lot of parallel load!
  let done = false;
  let l = [];
  let jobs = [];
  let counter = 0;
  let documentCount = 0;
  while (true) {
    if (!done) {
      while (l.length < batchSize) {
        let d = maker(counter); 
        if (d === null || d === false || d === undefined) {
          done = true;
          break;
        }
        if (Array.isArray(d)) {
          l = l.concat(d);
          documentCount += d.length;
        } else if (typeof(d) === "object") {
          l.push(d);
          documentCount += 1;
        }
        counter += 1;
        if (documentCount >= limit) {
          done = true;
        }
      }
    }
    if ((done && l.length > 0) || l.length >= batchSize) {
      jobs.push(arango.POST_RAW(`/_db/${encodeURIComponent(db)}/_api/document/${encodeURIComponent(coll)}`,
                                l, {"x-arango-async": "store"})
         .headers["x-arango-async-id"]);
      l = [];
    }
    let i = 0;
    while (i < jobs.length) {
      let r = arango.PUT_RAW(`/_api/job/${jobs[i]}`, {});
      if (r.code === 204) {
        i += 1;
      } else if (r.code === 202) {
        jobs = jobs.slice(0, i).concat(jobs.slice(i+1));
      }
    }
    if (done) {
      if (jobs.length === 0) {
        break;
      }
      require("internal").wait(0.5);
    }
  }
};

exports.executeExternalAndWaitWithSanitizer = function (executable, args, tmpFileName) {
  let sanHnd = new sanHandler(executable, global.instanceManager.options);
  let tmpMgr = new tmpDirMngr(fs.join(tmpFileName), global.instanceManager.options);
  sanHnd.detectLogfiles(tmpMgr.tempDir, tmpMgr.tempDir);
  let actualRc = internal.executeExternalAndWait(executable, args, false, 0, sanHnd.getSanOptions());
  sanHnd.fetchSanFileAfterExit(actualRc.pid);
  return actualRc;
};
