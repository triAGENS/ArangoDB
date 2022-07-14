/*jshint strict: false */

// //////////////////////////////////////////////////////////////////////////////
// / @brief Helper for JavaScript Tests
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Wilfried Goesgens
// / @author Copyright 2011-2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const internal = require('internal'); // OK: processCsvFile
const {
  getServerById,
  getServersByType,
  getEndpointById,
  getEndpointsByType,
  Helper,
  deriveTestSuite,
  deriveTestSuiteWithnamespace,
  typeName,
  isEqual,
  compareStringIds,
} = require('@arangodb/test-helper-common');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const _ = require('lodash');
const inst = require('@arangodb/testutils/instance');
const request = require('@arangodb/request');
const arangosh = require('@arangodb/arangosh');
const jsunity = require('jsunity');
const arango = internal.arango;
const db = internal.db;
const {assertTrue, assertFalse, assertEqual} = jsunity.jsUnity.assertions;

exports.getServerById = getServerById;
exports.getServersByType = getServersByType;
exports.getEndpointById = getEndpointById;
exports.getEndpointsByType = getEndpointsByType;
exports.Helper = Helper;
exports.deriveTestSuite = deriveTestSuite;
exports.deriveTestSuiteWithnamespace = deriveTestSuiteWithnamespace;
exports.typeName = typeName;
exports.isEqual = isEqual;
exports.compareStringIds = compareStringIds;

let reconnectRetry = exports.reconnectRetry = require('@arangodb/replication-common').reconnectRetry;

/// @brief set failure point
exports.debugCanUseFailAt = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    
    let res = arango.GET_RAW('/_admin/debug/failat');
    return res.code === 200;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

/// @brief set failure point
exports.debugSetFailAt = function (endpoint, failAt) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let res = arango.PUT_RAW('/_admin/debug/failat/' + failAt, {});
    if (res.parsedBody !== true) {
      throw "Error setting failure point + " + res;
    }
    return true;
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
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
    reconnectRetry(primaryEndpoint, "_system", "root", "");
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
    reconnectRetry(primaryEndpoint, "_system", "root", "");
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
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.debugGetFailurePoints = function (endpoint) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    let haveFailAt = arango.GET("/_admin/debug/failat") === true;
    if (haveFailAt) {
      let res = arango.GET_RAW('/_admin/debug/failat/all');
      if (res.code !== 200) {
        throw "Error checking failure points = " + JSON.stringify(res);
      }
      return res.parsedBody;
    }
  } finally {
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
  return [];
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
    reconnectRetry(primaryEndpoint, "_system", "root", "");
  }
};

exports.getRawMetric = function (endpoint, tags) {
  const primaryEndpoint = arango.getEndpoint();
  try {
    reconnectRetry(endpoint, db._name(), "root", "");
    return arango.GET_RAW('/_admin/metrics' + tags);
  } finally {
    reconnectRetry(primaryEndpoint, db._name(), "root", "");
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
  return Number(matches[0].replace(/^.*{.*}([0-9.]+)$/, "$1"));
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

const debug = function (text) {
  console.warn(text);
};

const runShell = function(args, prefix) {
  let options = internal.options();

  let endpoint = arango.getEndpoint().replace(/\+vpp/, '').replace(/^http:/, 'tcp:').replace(/^https:/, 'ssl:').replace(/^vst:/, 'tcp:').replace(/^h2:/, 'tcp:');
  let moreArgs = {
    'javascript.startup-directory': options['javascript.startup-directory'],
    'server.endpoint': endpoint,
    'server.database': arango.getDatabaseName(),
    'server.username': arango.connectedUser(),
    'server.password': '',
    'server.request-timeout': '10',
    'log.foreground-tty': 'false',
    'log.output': 'file://' + prefix + '.log'
  };
  _.assign(args, moreArgs);
  let argv = internal.toArgv(args);

  for (let o in options['javascript.module-directory']) {
    argv.push('--javascript.module-directory');
    argv.push(options['javascript.module-directory'][o]);
  }

  let result = internal.executeExternal(global.ARANGOSH_BIN, argv, false /*usePipes*/);
  assertTrue(result.hasOwnProperty('pid'));
  let status = internal.statusExternal(result.pid);
  assertEqual(status.status, "RUNNING");
  return result.pid;
};

const buildCode = function(key, command, cn) {
  let file = fs.getTempFile() + "-" + key;
  fs.write(file, `
(function() {
let tries = 0;
while (true) {
  if (++tries % 3 === 0) {
    if (db['${cn}'].exists('stop')) {
      break;
    }
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

  let args = {'javascript.execute': file};
  let pid = runShell(args, file);
  debug("started client with key '" + key + "', pid " + pid + ", args: " + JSON.stringify(args));
  return { key, file, pid };
};
exports.runShell = runShell;

exports.runParallelArangoshTests = function (tests, duration, cn) {
  assertTrue(fs.isFile(global.ARANGOSH_BIN), "arangosh executable not found!");
  
  assertFalse(db[cn].exists("stop"));
  let clients = [];
  debug("starting " + tests.length + " test clients");
  try {
    tests.forEach(function (test) {
      let key = test[0];
      let code = test[1];
      let client = buildCode(key, code, cn);
      client.done = false;
      client.failed = true; // assume the worst
      clients.push(client);
    });

    debug("running test for " + duration + " s...");

    internal.sleep(duration);

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
    let tries = 0;
    const allClientsDone = () => clients.every(client => client.done);
    while (++tries < 120) {
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
        fs.remove(client.file);
      } catch (err) { }

      const logfile = client.file + '.log';
      if (client.failed) {
        if (fs.exists(logfile)) {
          debug("test client with pid " + client.pid + " has failed and wrote logfile: " + fs.readFileSync(logfile).toString());
        } else {
          debug("test client with pid " + client.pid + " has failed and did not write a logfile");
        }
      }
      try {
        fs.remove(logfile);
      } catch (err) { }

      if (!client.done) {
        // hard-kill all running instances
        try {
          let status = internal.statusExternal(client.pid).status;
          if (status === 'RUNNING') {
            debug("forcefully killing test client with pid " + client.pid);
            internal.killExternal(client.pid, 9 /*SIGKILL*/);
          }
        } catch (err) { }
      }
    });
  }
  return clients;
};

exports.waitForShardsInSync = function (cn, timeout, minimumRequiredFollowers = 0) {
  if (!timeout) {
    timeout = 300;
  }
  let start = internal.time();
  while (true) {
    if (internal.time() - start > timeout) {
      assertTrue(false, "Shards were not getting in sync in time, giving up!");
      return;
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
  const instanceInfo = JSON.parse(require('internal').env.INSTANCEINFO);
  let ret = instanceInfo.arangods.filter(inst => inst.instanceRole === role);
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

exports.getEndpoints = function (role) {
  const endpointToURL = (instance) => {
    let protocol = instance.endpoint.split('://')[0];
    switch(protocol) {
    case 'ssl':
      return 'https://' + instance.endpoint.substr(6);
      break;
    case 'tcp':
      return 'http://' + instance.endpoint.substr(6);
      break;
    default:
      var pos = instance.endpoint.indexOf('://');
      if (pos === -1) {
        return 'http://' + instance.endpoint;
      }
      return 'http' + instance.endpoint.substr(pos);
    }
  };
  return exports.getServers(role).map(endpointToURL);
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

exports.agency = {
  get: function (path) {
    const agents = exports.getAgentEndpoints();
    assertTrue(agents.length > 0, 'No agents present');
    var res = request.post({
      url: agents[0] + '/_api/agency/read',
      body: JSON.stringify([[`/arango/${path}`]]),
      timeout: 300,
    });
    assertTrue(res instanceof request.Response);
    assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
    assertEqual(res.statusCode, 200, JSON.stringify(res));
    assertTrue(res.hasOwnProperty('json'));
    return arangosh.checkRequestResult(res.json)[0];
  },

  set: function (path, value) {
    const agents = exports.getAgentEndpoints();
    assertTrue(agents.length > 0, 'No agents present');
    var res = request.post({
      url: agents[0] + '/_api/agency/write',
      body: JSON.stringify([[{
        [`/arango/${path}`]: {
          'op': 'set',
          'new': value,
        },
      }]]),
      timeout: 300,
    });
    assertTrue(res instanceof request.Response);
    assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
    assertEqual(res.statusCode, 200, JSON.stringify(res));
    assertTrue(res.hasOwnProperty('json'));
    return arangosh.checkRequestResult(res.json)[0];
  },

  increaseVersion: function (path) {
    const agents = exports.getAgentEndpoints();
    assertTrue(agents.length > 0, 'No agents present');
    var res = request.post({
      url: agents[0] + '/_api/agency/write',
      body: JSON.stringify([[{[`/arango/${path}`]: {'op': 'increment'}}]]),
      timeout: 300,
    });
    assertTrue(res instanceof request.Response);
    assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
    assertEqual(res.statusCode, 200, JSON.stringify(res));
    assertTrue(res.hasOwnProperty('json'));
    return arangosh.checkRequestResult(res.json)[0];
  },
};