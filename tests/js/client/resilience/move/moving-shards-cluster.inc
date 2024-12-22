/*jshint globalstrict:false, strict:false */
/*global assertTrue, assertEqual, ArangoAgency, ArangoClusterInfo */
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
/// @author Max Neunhoeffer
/// @author Copyright 2016, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");

const arangodb = require("@arangodb");
const db = arangodb.db;
const _ = require("lodash");
const internal = require("internal");
const {errors, wait} = internal;
const supervisionState = require("@arangodb/testutils/cluster-test-helper").supervisionState;
const queryAgencyJob = require("@arangodb/testutils/cluster-test-helper").queryAgencyJob;
const {getServersByType, deriveTestSuite, getUrlById} = require('@arangodb/test-helper-common');
const request = require('@arangodb/request');
const helper = require("@arangodb/testutils/replicated-logs-helper");
const lpreds = require("@arangodb/testutils/replicated-logs-predicates");
const {
  agency,
  arangoClusterInfoGetCollectionInfo,
  arangoClusterInfoGetCollectionInfoCurrent,
  arangoClusterInfoFlush,
  getDBServers,
  getAgents,
  getEndpointById,
  getCoordinators
} = require('@arangodb/test-helper');
const { instanceRole } = require("@arangodb/testutils/instance");
let IM = global.instanceManager;

// in the `useData` case, use this many documents:
const numDocuments = 1000;

const dbservers = getDBServers().map(arangod => arangod.id);

function delaySupervisionFailoverActions(value) {
  let agents = getAgents().map(
    arangod => arangod.url);
  for (let a of agents) {
    const res = request({url: a + "/_api/agency/config",
                   method: "PUT",
                   body: JSON.stringify(
                     {delayAddFollower: value, delayFailedFollower: value})});
    assertEqual(200, res.statusCode);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

exports.MovingShardsSuite = function({useData, replVersion}) {
  if (typeof useData !== 'boolean') {
    throw new Error('MovingShardsSuite expects its parameter `useData` to be set and a boolean!');
  }
  const cn = "UnitTestMovingShards";
  const dbn = "UnitTestMovingShards_V" + replVersion;
  var count = 0;
  var c = [];

////////////////////////////////////////////////////////////////////////////////
/// @brief find out servers for a collection
////////////////////////////////////////////////////////////////////////////////

  function findCollectionServers(database, collection) {
    var cinfo = arangoClusterInfoGetCollectionInfo(database, collection);
    var shard = Object.keys(cinfo.shards)[0];

    if (replVersion === "2") {
      var shardsToLogs = helper.getShardsToLogsMapping(database, cinfo.id);
      const id = shardsToLogs[shard];
      const spec = db._replicatedLog(id).status().specification.plan;
      let servers = Object.keys(spec.participantsConfig.participants);
      // make leader first server
      if (spec.currentTerm && spec.currentTerm.leader) {
        const leader = spec.currentTerm.leader.serverId;
        let index = servers.indexOf(leader);
        if (index !== -1) {
          servers.splice(index, 1);
        }
        servers.unshift(leader);
      }
      return servers;
    } else {
      return cinfo.shards[shard];
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief wait for synchronous replication
////////////////////////////////////////////////////////////////////////////////

  function waitForSynchronousReplication(database) {
    console.info("Waiting for synchronous replication to settle...");
    arangoClusterInfoFlush();
    for (var i = 0; i < c.length; ++i) {
      var cinfo = arangoClusterInfoGetCollectionInfo(
          database, c[i].name());
      var shards = Object.keys(cinfo.shards);
      var replFactor = cinfo.shards[shards[0]].length;
      var count = 0;
      while (++count <= 180) {
        var ccinfo = shards.map(
            s => arangoClusterInfoGetCollectionInfoCurrent(
                database, c[i].name(), s)
        );
        var replicas = ccinfo.map(s => [s.servers.length, s.failoverCandidates.length]);
        if (replicas.every(x => x[0] === replFactor && x[0] === x[1])) {
          // This also checks that there are as many failoverCandidates
          // as there are followers in sync. This should eventually be
          // reached.
          console.info("Replication up and running!");
          break;
        }
        wait(0.5);
        arangoClusterInfoFlush();
      }
      if (count > 120) {
        return false;
      }
    }
    return true;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief wait for an incomplete moveShard to have happened, this is a
/// very special test for "testMoveShardFromFollowerRepl3_failoverCands".
////////////////////////////////////////////////////////////////////////////////

  function waitForIncompleteMoveShard(database, collection, replFactor) {
    console.info("Waiting for incomplete move shard to settle...");
    arangoClusterInfoFlush();
    var cinfo = arangoClusterInfoGetCollectionInfo(database, collection);
    var shards = Object.keys(cinfo.shards);
    var count = 0;
    while (++count <= 180) {
      var ccinfo = shards.map(
          s => arangoClusterInfoGetCollectionInfoCurrent(
              database, collection, s)
      );
      var replicas = ccinfo.map(s => [s.servers.length, s.failoverCandidates.length]);
      if (replicas.every(x => x[0] === replFactor + 1 && x[0] === x[1])) {
        // This also checks that there are as many failoverCandidates
        // as there are followers in sync. This should eventually be
        // reached.
        console.info("Incomplete moveShard completed!");
        break;
      }
      wait(0.5);
      arangoClusterInfoFlush();
    }
    if (count > 120) {
      return false;
    }
    return true;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief get cleaned out servers
////////////////////////////////////////////////////////////////////////////////

  function getCleanedOutServers() {
    var coordEndpoint =
        getCoordinators()[0];

    var request = require("@arangodb/request");
    var url = coordEndpoint.url;


    var res;
    try {
      var envelope =
          {method: "GET", url: url + "/_admin/cluster/numberOfServers"};
      res = request(envelope);
    } catch (err) {
      console.error(
          "Exception for GET /_admin/cluster/cleanOutServer:", err.stack);
      return {cleanedServers: []};
    }
    if (res.statusCode !== 200) {
      return {cleanedServers: []};
    }
    var body = res.body;
    if (typeof body === "string") {
      try {
        body = JSON.parse(body);
      } catch (err2) {
      }
    }
    if (typeof body !== "object" || !body.hasOwnProperty("cleanedServers") ||
        typeof body.cleanedServers !== "object") {
      return {cleanedServers: []};
    }
    return body;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief display agency information in case of a bad outcome
////////////////////////////////////////////////////////////////////////////////

  function displayAgencyInformation() {
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;

    var res;
    try {
      var envelope = {method: "GET", url: url + "/_api/cluster/agency-dump"};
      res = request(envelope);
    } catch (err) {
      console.error(
          "Exception for GET /_api/cluster/agency-dump:", err.stack);
      return;
    }
    if (res.statusCode !== 200) {
      return;
    }
    var body = res.body;
    console.error("Agency state after disaster:", body);
  }

  function testServerNoLeader(id, fromCollNr, toCollNr) {
    if (fromCollNr === undefined) {
      fromCollNr = 0;
    }
    if (toCollNr === undefined) {
      toCollNr = c.length - 1;
    }
    for (var i = fromCollNr; i <= toCollNr; ++i) {
      arangoClusterInfoFlush();
      var servers = findCollectionServers(dbn, c[i].name());
      if (servers.indexOf(id) === 0 && servers.length !== 1) {
        return false;
      }

      // Now check current as well:
      if (replVersion !== "2") {
        var collInfo =
            arangoClusterInfoGetCollectionInfo(dbn, c[i].name());
        var shards = collInfo.shards;
        var collInfoCurr =
            Object.keys(shards).map(
                s => arangoClusterInfoGetCollectionInfoCurrent(
                    dbn, c[i].name(), s).servers);
        // quick hack here, if the list of servers has length one, we map it to -1
        // thus the check below will not return false in the case replFact === 1
        var idxs = collInfoCurr.map(l => (l.length > 1) ? l.indexOf(id) : -1);
        for (var j = 0; j < idxs.length; j++) {
          if (idxs[j] === 0) {
            return false;
          }
        }
      }
    }
    return true;
  }

  function findLeaderShardsForServer(id) {
    let result = [];
    for (var i = 0; i <  c.length ; ++i) {
      arangoClusterInfoFlush();
      var servers = findCollectionServers(dbn, c[i].name());
      if (servers.indexOf(id) === 0 && servers.length !== 1) {
        result.push(c[i].name());
      }
    }
    return result;
  }

  function waitForShardLeader(id, expectedShards) {
    helper.waitFor(function () {
      const shards = findLeaderShardsForServer(id);
      if (!_.isEqual(shards, expectedShards)) {
        return Error(`expected shards to be ${expectedShards}, but found ${shards}`);
      }
      return true;
    });
    return true;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief test whether or not a server is clean
////////////////////////////////////////////////////////////////////////////////

  function testServerEmpty(id, checkList, fromCollNr, toCollNr) {
    if (fromCollNr === undefined) {
      fromCollNr = 0;
    }
    if (toCollNr === undefined) {
      toCollNr = c.length - 1;
    }
    var count = 600;
    var ok = false;

    console.info("Waiting for server " + id + " to be cleaned out ...");

    if (checkList) {

      // Wait until the server appears in the list of cleanedOutServers:
      var obj;
      while (--count > 0) {
        obj = getCleanedOutServers();
        if (obj.cleanedServers.indexOf(id) >= 0) {
          ok = true;
          console.info(
              "Success: Server " + id + " cleaned out after " + (600 - count) + " seconds");
          break;
        }
        wait(1.0);
      }

      if (!ok) {
        console.info(
            "Failed: Server " + id + " was not cleaned out. List of cleaned servers: ["
            + obj.cleanedServers + "]");
        displayAgencyInformation();
      }

    } else {

      for (var i = fromCollNr; i <= toCollNr; ++i) {

        while (--count > 0) {
          wait(1.0);
          arangoClusterInfoFlush();
          var servers = findCollectionServers(dbn, c[i].name());
          if (servers.indexOf(id) === -1) {
            // Now check current as well:
            /*var collInfo =
                arangoClusterInfoGetCollectionInfo(dbn, c[i].name());
            var shards = collInfo.shards;
            var collInfoCurr =
                Object.keys(shards).map(
                  s => arangoClusterInfoGetCollectionInfoCurrent(
                    dbn, c[i].name(), s).servers);
            var idxs = collInfoCurr.map(l => l.indexOf(id));
            ok = true;
            for (var j = 0; j < idxs.length; j++) {
              if (idxs[j] !== -1) {
                ok = false;
              }
            }
            if (ok) {
              break;
            }*/
            ok = true;
            break;
          }
        }
        if (!ok) {
          displayAgencyInformation();
          return false;
        }

      }

    }
    return ok;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief order the cluster to clean out a server:
////////////////////////////////////////////////////////////////////////////////

  function cleanOutServer(id, dontwait = false) {
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var body = {"server": id};
    var result;
    try {
      result = request({
        method: "POST",
        url: url + "/_admin/cluster/cleanOutServer",
        body: JSON.stringify(body)
      });
    } catch (err) {
      console.error(
          "Exception for POST /_admin/cluster/cleanOutServer:", err.stack);
      return false;
    }
    console.info("cleanOutServer job:", JSON.stringify(body));
    console.info("result of request:", JSON.stringify(result.json));
    if (dontwait) {
      return result;
    }
    // Now wait until the job we triggered is finished:
    var count = 1200;   // seconds
    while (true) {
      var job = queryAgencyJob(result.json.id);
      console.info("Status of cleanOutServer job:", job.status);
      if (job.error === false) {
        if (job.status === "Finished") {
          return result;
        } else if (job.status === "Failed") {
          return false;
        }
      }
      if (count-- < 0) {
        console.error(
            "Timeout in waiting for cleanOutServer to complete: "
            + JSON.stringify(body));
        return false;
      }
      wait(1.0);
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief request a dbserver to resign:
////////////////////////////////////////////////////////////////////////////////

  function resignLeadership(id, options = {}) {
    const {undoMoves = false, waitForInSync = false, waitForInSyncTimeout = 60} = options;
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var body = {"server": id, undoMoves, waitForInSync, waitForInSyncTimeout};
    var result;
    try {
      result = request({
        method: "POST",
        url: url + "/_admin/cluster/resignLeadership",
        body: JSON.stringify(body)
      });
    } catch (err) {
      console.error(
          "Exception for POST /_admin/cluster/resignLeadership:", err.stack);
      return false;
    }
    console.info("resignLeadership job:", JSON.stringify(body));
    console.info("result of request:", JSON.stringify(result.json));
    // Now wait until the job we triggered is finished:
    var count = 1200;   // seconds
    while (true) {
      var job = queryAgencyJob(result.json.id);
      console.info("Status of resignLeadership job:", job.status);
      if (job.error === false && job.status === "Finished") {
        return result;
      }
      if (count-- < 0) {
        console.error(
            "Timeout in waiting for resignLeadership to complete: "
            + JSON.stringify(body));
        return false;
      }
      wait(1.0);
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief order the cluster to reduce number of db servers
////////////////////////////////////////////////////////////////////////////////

  function shrinkCluster(toNum) {
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var body = {"numberOfDBServers": toNum};
    let res = {};
    try {
      res = request({
        method: "PUT",
        url: url + "/_admin/cluster/numberOfServers",
        body: JSON.stringify(body)
      });
    } catch (err) {
      console.error(
          "Exception for PUT /_admin/cluster/numberOfServers:", err.stack);
      return false;
    }
    return res.hasOwnProperty("statusCode") && res.statusCode === 200;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief order the cluster to clean out a server:
////////////////////////////////////////////////////////////////////////////////

  function resetCleanedOutServers() {
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var numberOfDBServers = dbservers.length;
    var body = {"cleanedServers": [], "numberOfDBServers": numberOfDBServers};
    try {
      var res = request({
        method: "PUT",
        url: url + "/_admin/cluster/numberOfServers",
        body: JSON.stringify(body)
      });
      // To ensure that all our favourite Coordinator0001 has actually
      // updated its agencyCache to reflect the newly empty CleanedServers
      // list, we ask him to create a collection with replicationFactor
      // numberOfDBServers. If this works, we can get rid of the collection
      // again and all is well. If it does not work, we have to wait a bit
      // longer:
      for (var count = 0; count < 120; ++count) {
        try {
          var c = db._create("collectionProbe", {replicationFactor: numberOfDBServers});
          c.drop();
          return res;
        } catch (err2) {
          if (err2.errorNum !== errors.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code) {
            console.error("Got unexpected error from collection probe: ", err2.stack);
            return false;
          }
        }
        console.log("Collection probe unsuccessful for replicationFactor=", numberOfDBServers, " count=", count);
        wait(0.5);
      }
      console.error("Could not successfully create collection probe for 60s, giving up!");
      return false;
    } catch (err) {
      console.error(
          "Exception for PUT /_admin/cluster/numberOfServers:", err.stack);
      return false;
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief move a single shard
////////////////////////////////////////////////////////////////////////////////

  function moveShard(database, collection, shard, fromServer, toServer, dontwait, expectedResult) {
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var body = {database, collection, shard, fromServer, toServer};
    var result;
    try {
      result = request({
        method: "POST",
        url: url + "/_admin/cluster/moveShard",
        body: JSON.stringify(body)
      });
    } catch (err) {
      console.error(
          "Exception for PUT /_admin/cluster/moveShard:", err.stack);
      return false;
    }
    console.info("moveShard job:", JSON.stringify(body));
    console.info("result of request:", JSON.stringify(result.json));
    if (dontwait) {
      return result;
    }
    // Now wait until the job we triggered is finished:
    var count = 600;   // seconds
    while (true) {
      var job = queryAgencyJob(result.json.id);
      console.info("Status of moveShard job:", job.status);
      if (job.error === false && job.status === expectedResult) {
        return result;
      }
      if (count-- < 0) {
        console.error(
            "Timeout in waiting for moveShard to complete: "
            + JSON.stringify(body));
        return false;
      }
      wait(1.0);
    }
  }


////////////////////////////////////////////////////////////////////////////////
/// @brief Set supervision mode
////////////////////////////////////////////////////////////////////////////////

  function maintenanceMode(mode) {
    console.log("Switching supervision maintenance " + mode);
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var req;
    try {
      req = request({
        method: "PUT",
        url: url + "/_admin/cluster/maintenance",
        body: JSON.stringify(mode)
      });
    } catch (err) {
      console.error(
          "Exception for PUT /_admin/cluster/maintenance:", err.stack);
      return false;
    }
    console.log("Supervision maintenance is " + mode);
    return true;
  }


////////////////////////////////////////////////////////////////////////////////
/// @brief Abort job
////////////////////////////////////////////////////////////////////////////////

  function cancelAgencyJob(id) {
    console.log("Killing job " + id);
    var coordEndpoint =
        getCoordinators()[0];
    var request = require("@arangodb/request");
    var url = coordEndpoint.url;
    var req;
    for (let i = 0; i < 10; i++) {
      try {
        req = request({
          method: "POST",
          url: url + "/_admin/cluster/cancelAgencyJob",
          body: JSON.stringify({id: id})
        });
      } catch (err) {
        console.error(
            "Exception for POS /_admin/cluster/cancelAgencyJob:" + {id: id}, err.stack);
        return false;
      }

      const result = JSON.parse(req.body);
      if (result.error === false) {
        return true;
      } else if (result.code !== 404) {
        console.error("Failed to cancel job " + id + ": " + JSON.stringify(result));
        return false;
      } else {
        console.log("Job not yet detected by coordinator");
      }

      wait(1.0);
    }
    console.error("Failed to cancel job" + id + ", job not visible to coordinator.");
    return false;
  }


////////////////////////////////////////////////////////////////////////////////
/// @brief create some collections
////////////////////////////////////////////////////////////////////////////////

  function createSomeCollections(n, nrShards, replFactor, useData, otherNumDocuments = 0) {
    assertEqual('boolean', typeof useData);
    var systemCollServers = findCollectionServers(dbn, "_graphs");
    console.info("System collections use servers:", systemCollServers);
    for (var i = 0; i < n; ++i) {
      ++count;
      while (true) {
        var name = cn + count;
        db._drop(name);
        var coll = db._create(name, {
          numberOfShards: nrShards,
          replicationFactor: replFactor,
          avoidServers: systemCollServers
        });

        if (useData) {
          // insert some documents
          let nd = (otherNumDocuments === 0) ? numDocuments : otherNumDocuments;
          coll.insert(_.range(0, nd).map(v => ({value: v, x: "someString" + v})));
        }

        var servers = findCollectionServers(dbn, name);
        console.info("Test collection uses servers:", servers);
        if (_.intersection(systemCollServers, servers).length === 0) {
          c.push(coll);
          break;
        }
        console.info("Need to recreate collection to avoid system collection servers.");
        c.push(coll);
        waitForSynchronousReplication(dbn);
        c.pop();
        console.info("Synchronous replication has settled, now dropping again.");
      }
    }
  }

  function checkCollectionContents(otherNumDocuments = 0) {
    if (replVersion === "2") {
      return; // TODO
    }
    let nd = (otherNumDocuments === 0) ? numDocuments : otherNumDocuments;
    const numDocs = useData ? nd : 0;
    for (const collection of c) {
      assertEqual(numDocs, collection.count());
      const values = db._query(
          'FOR doc IN @@col SORT doc.value RETURN doc.value',
          {'@col': collection.name()}).toArray();
      for (const v of _.range(0, numDocs)) {
        assertEqual(v, values[v], values);
      }
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief find server not on a list
////////////////////////////////////////////////////////////////////////////////

  function findServerNotOnList(list) {
    var count = 0;
    while (list.indexOf(dbservers[count]) >= 0) {
      count += 1;
    }
    return dbservers[count];
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief wait for Supervision to finish jobs
////////////////////////////////////////////////////////////////////////////////

  function waitForSupervision() {
    var count = 300;
    while (--count > 0) {
      var state = supervisionState();
      if (!state.error &&
          Object.keys(state.ToDo).length === 0 &&
          Object.keys(state.Pending).length === 0) {
        return true;
      }
      if (state.error) {
        console.warn("Waiting for supervision jobs to finish:",
            "Currently no agency communication possible.");
      } else {
        console.info("Waiting for supervision jobs to finish:",
            "ToDo jobs:", Object.keys(state.ToDo).length,
            "Pending jobs:", Object.keys(state.Pending).length);
      }
      wait(1.0);
    }
    return false;
  }

  function getServerRebootId(server) {
    return helper.readAgencyValueAt(`Current/ServersKnown/${server}/rebootId`);
  }

  function waitForRebootIdChanged(server, oldRebootId) {

    var count = 300;
    while(--count > 0) {
      if (getServerRebootId(server) !== oldRebootId) {
        return;
      }
      wait(1.0);
    }
    assertTrue(count > 0);
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief the actual tests
////////////////////////////////////////////////////////////////////////////////

  return {

    setUpAll: function () {
      db._createDatabase(dbn, {replicationVersion: replVersion});
      db._useDatabase(dbn);
    },

    tearDownAll: function () {
      db._useDatabase("_system");
      db._dropDatabase(dbn);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief set up
////////////////////////////////////////////////////////////////////////////////

    setUp: function () {
      createSomeCollections(1, 1, 2, useData);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief tear down
////////////////////////////////////////////////////////////////////////////////

    tearDown: function () {
      for (var i = 0; i < c.length; ++i) {
        c[i].drop();
      }
      c = [];
      resetCleanedOutServers();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a synchronously replicated collection gets online
////////////////////////////////////////////////////////////////////////////////

    testSetup: function () {
      for (var count = 0; count < 120; ++count) {
        if (dbservers.length === 5) {
          assertTrue(waitForSynchronousReplication(dbn));
          checkCollectionContents();

          return;
        }
        console.log("Waiting for 5 dbservers to be present:", JSON.stringify(dbservers));
        wait(1.0);
      }
      assertTrue(false, "Timeout waiting for 5 dbservers.");
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out collection with one shard without replication
////////////////////////////////////////////////////////////////////////////////

    testShrinkNoReplication: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var _dbservers = _.clone(dbservers);
      _dbservers.sort();
      assertTrue(shrinkCluster(4));
      assertTrue(testServerEmpty(_dbservers[4], true));
      assertTrue(waitForSupervision());
      assertTrue(shrinkCluster(3));
      assertTrue(testServerEmpty(_dbservers[3], true));
      assertTrue(waitForSupervision());
      assertTrue(shrinkCluster(2));
      assertTrue(testServerEmpty(_dbservers[2], true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a follower
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromFollower: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var fromServer = servers[1];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[0].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[0]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer), false);
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a leader
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromLeader: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var fromServer = servers[0];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[0].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[0]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer), false);
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a leader without replication
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromLeaderNoReplication: function () {
      createSomeCollections(1, 1, 1, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer, false, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a follower with 3 replicas #1
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromFollowerRepl3_1: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[1];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer, false, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a follower with 3 replicas #2
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromRepl3_2: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[2];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer, false, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a leader with 3 replicas
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromLeaderRepl: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerEmpty(fromServer, false, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief moving away a shard from a leader to a follower
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromLeaderToFollower: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var toServer = servers[1];
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
      assertTrue(testServerNoLeader(fromServer, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },


////////////////////////////////////////////////////////////////////////////////
/// @brief resign leadership for a dbserver
////////////////////////////////////////////////////////////////////////////////

    testResignLeadership: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var toResign = servers[0];
      assertTrue(resignLeadership(toResign));
      assertTrue(testServerNoLeader(toResign));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief resign leadership for a dbserver but wait for in sync
////////////////////////////////////////////////////////////////////////////////

    testResignLeadershipWaitForInSync: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var toResign = servers[0];
      assertTrue(resignLeadership(toResign,
          {waitForInSync: true}));
      assertTrue(testServerNoLeader(toResign));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief resign leadership for a dbserver but wait for in sync with timeout
////////////////////////////////////////////////////////////////////////////////

    testResignLeadershipWaitForInSyncTimeout: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var toResign = servers[0];
      assertTrue(resignLeadership(toResign,
          {waitForInSync: true, waitForInSyncTimeout: 5}));
      assertTrue(testServerNoLeader(toResign));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },


////////////////////////////////////////////////////////////////////////////////
/// @brief resign leadership for a dbserver and wait for restore
////////////////////////////////////////////////////////////////////////////////

    testResignLeadershipWithUndo: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      try {
        delaySupervisionFailoverActions(120);

        var servers = findCollectionServers(dbn, c[0].name());
        var toResign = servers[0];
        const shards = findLeaderShardsForServer(toResign);
        assertTrue(resignLeadership(toResign, {undoMoves: true}));
        assertTrue(testServerNoLeader(toResign));
        assertTrue(waitForSupervision());

        checkCollectionContents();

        // get server reboot id
        const rebootId = getServerRebootId(toResign);

        // now suspend that server
        helper.stopServerWaitFailed(toResign);

        // Wait for the reboot id to be changed.
        waitForRebootIdChanged(toResign, rebootId);

        // restart the server
        helper.continueServerWaitOk(toResign);

        // now wait for the server to become leader again for shards
        assertTrue(waitForShardLeader(toResign, shards));
        assertTrue(waitForSupervision());
        checkCollectionContents();
      } finally {
        delaySupervisionFailoverActions(0);
      }
    },


////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out a follower
////////////////////////////////////////////////////////////////////////////////

    testCleanOutFollower: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var toClean = servers[1];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out a leader
////////////////////////////////////////////////////////////////////////////////

    testCleanOutLeader: function () {
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[0].name());
      var toClean = servers[0];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out with multiple collections
////////////////////////////////////////////////////////////////////////////////

    testCleanOutMultipleCollections: function () {
      createSomeCollections(10, 1, 2, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var toClean = servers[0];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out with a collection with 3 replicas
////////////////////////////////////////////////////////////////////////////////

    testCleanOut3Replicas: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var toClean = servers[0];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out collection with multiple shards
////////////////////////////////////////////////////////////////////////////////

    testCleanOutMultipleShards: function () {
      createSomeCollections(1, 10, 2, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var toClean = servers[1];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief cleaning out collection with one shard without replication
////////////////////////////////////////////////////////////////////////////////

    testCleanOutNoReplication: function () {
      createSomeCollections(1, 1, 1, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var toClean = servers[0];
      assertTrue(cleanOutServer(toClean));
      assertTrue(testServerEmpty(toClean, true));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief pausing supervision for a couple of seconds
////////////////////////////////////////////////////////////////////////////////

    testMaintenanceMode: function () {
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(maintenanceMode("on"));
      assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, true, "Finished"));
      var first = agency.call("transient", [["/arango/Supervision/State"]])[0].arango.Supervision.State, state;
      var waitUntil = new Date().getTime() + 30.0 * 1000;
      while (true) {
        state = agency.call("transient", [["/arango/Supervision/State"]])[0].arango.Supervision.State;
        assertEqual(state.Timestamp, first.Timestamp);
        wait(5.0);
        if (new Date().getTime() > waitUntil) {
          break;
        }
      }
      assertTrue(maintenanceMode("off"));
      state =agency.call("transient", [["/arango/Supervision/State"]])[0].arango.Supervision.State;
      assertTrue(state.Timestamp !== first.Timestamp);
      assertTrue(testServerEmpty(fromServer, false, 1, 1));
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief pausing supervision for a couple of seconds
////////////////////////////////////////////////////////////////////////////////

    testMoveShardFromFollowerRepl3_failoverCands: function () {
      if (!IM.debugCanUseFailAt()) {
        console.log("Skipping test for failoverCandidates because failure points are not compiled in!");
        return;
      }
      if (replVersion === "2") {
        console.log("Skipping failure point test for replication 2, because there is no such failure point");
        return;
      }
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var leader = servers[0];
      var fromServer = servers[1];
      var leaderUrl = getUrlById(leader);
      // Switch off something in the maintenance on the leader to detect
      // followers which are not in Plan. This means that the moveShard
      // below will leave the old server in Current/servers and
      // Current/failoverCandidates.
      try {
        IM.debugSetFailAt("Maintenance::doNotRemoveUnPlannedFollowers", instanceRole.dbServer, leaderUrl);

        var toServer = findServerNotOnList(servers);
        var cinfo = arangoClusterInfoGetCollectionInfo(
            dbn, c[1].name());
        var shard = Object.keys(cinfo.shards)[0];
        assertTrue(moveShard(dbn, c[1]._id, shard, fromServer, toServer, false, "Finished"));
        assertTrue(waitForIncompleteMoveShard(dbn, c[1].name(), 3));
        wait(5);   // After 5 seconds the situation should be unchanged!
        assertTrue(waitForIncompleteMoveShard(dbn, c[1].name(), 3));
        // Now we know that the old follower is not in the plan but is in
        // failoverCandidates (and indeed in Current/servers). Let's now
        // try to move the shard back, this ought to be denied:
        assertTrue(moveShard(dbn, c[1]._id, shard, toServer, fromServer, false, "Failed"));
        IM.debugClearFailAt();
        // Now we should go back to only 3 servers in Current.
        assertTrue(waitForSynchronousReplication(dbn));
        assertTrue(testServerEmpty(fromServer, false, 1, 1));
        assertTrue(waitForSupervision());
        checkCollectionContents();
      } catch (err) {
        IM.debugClearFailAt();
        throw err;
      }
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief kill todo moveShard job
////////////////////////////////////////////////////////////////////////////////

    testCancelToDoMoveShard: function () {
      if (replVersion === "2") {
        console.log("Skipping cancel test for replication 2, because move shards are not cancellable.");
        return;
      }
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var toServer = findServerNotOnList(servers);
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(maintenanceMode("on"));
      let result = moveShard(dbn, c[1]._id, shard, fromServer, toServer, true, "Finished");
      assertEqual(result.statusCode, 202);
      let id = JSON.parse(result.body).id;
      assertTrue(cancelAgencyJob(id));
      assertTrue(maintenanceMode("off"));
      assertTrue(waitForSupervision());
      var job = queryAgencyJob(result.json.id);
      assertEqual(job.status, "Failed");
      assertEqual(job.abort);
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief kill pending moveShard job
////////////////////////////////////////////////////////////////////////////////

    testCancelPendingMoveShard: function () {
      if (replVersion === "2") {
        console.log("Skipping cancel test for replication 2, because move shards are not cancellable.");
        return;
      }
      if (useData) {
        for (var i = 0; i < c.length; ++i) {
          c[i].drop();
        }
        c = [];
        let otherNumDocuments = 100000;
        createSomeCollections(1, 1, 3, useData, otherNumDocuments);
        assertTrue(waitForSynchronousReplication(dbn));
        var servers = findCollectionServers(dbn, c[0].name());
        var fromServer = servers[0];
        var toServer = findServerNotOnList(servers);
        var cinfo = arangoClusterInfoGetCollectionInfo(
            dbn, c[0].name());
        var shard = Object.keys(cinfo.shards)[0];
        let result = moveShard(dbn, c[0]._id, shard, fromServer, toServer, true, "Finished");
        assertEqual(result.statusCode, 202);
        let id = JSON.parse(result.body).id;
        while (queryAgencyJob(result.json.id).status === "ToDo") { // wait for job to start
          wait(0.1);
        }
        assertTrue(cancelAgencyJob(id));
        while (queryAgencyJob(result.json.id).status === "Pending") { // wait for job to be killed
          wait(0.1);
        }
        var job = queryAgencyJob(result.json.id);
        assertEqual(job.status, "Failed");
        assertEqual(job.abort);
        assertTrue(waitForSupervision());
        checkCollectionContents(otherNumDocuments);
      }
    },

    testCancelPendingMoveShardDropCollection: function () {
      if (replVersion === "2") {
        console.log("Skipping cancel test for replication 2, because move shards are not cancellable.");
        return;
      }
      if (useData) {
        for (var i = 0; i < c.length; ++i) {
          c[i].drop();
        }
        c = [];
        let otherNumDocuments = 100000;
        createSomeCollections(1, 1, 3, useData, otherNumDocuments);
        assertTrue(waitForSynchronousReplication(dbn));
        var servers = findCollectionServers(dbn, c[0].name());
        var fromServer = servers[0];
        var toServer = findServerNotOnList(servers);
        var cinfo = arangoClusterInfoGetCollectionInfo(
            dbn, c[0].name());
        var shard = Object.keys(cinfo.shards)[0];
        let result = moveShard(dbn, c[0]._id, shard, fromServer, toServer, true, "Finished");
        assertEqual(result.statusCode, 202);
        let id = JSON.parse(result.body).id;
        while (queryAgencyJob(result.json.id).status === "ToDo") { // wait for job to start
          wait(0.1);
        }
        assertTrue(maintenanceMode("on"));
        assertEqual(queryAgencyJob(result.json.id).status, "Pending");
        assertTrue(cancelAgencyJob(id));
        for (var j = 0; j < c.length; ++j) {
          c[j].drop();
        }
        c = [];
        assertTrue(maintenanceMode("off"));
        while (queryAgencyJob(result.json.id).status === "Finish") { // wait for job to finish
          wait(0.1);
        }
        var job = queryAgencyJob(result.json.id);
        assertEqual(job.status, "Failed");
        assertEqual(job.abort);
        assertTrue(waitForSupervision());
        checkCollectionContents(otherNumDocuments);
      }
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief kill todo cleanOutServer job
////////////////////////////////////////////////////////////////////////////////

    testCancelToDoCleanOutServer: function () {
      if (replVersion === "2") {
        console.log("Skipping cancel test for replication 2, because move shards are not cancellable.");
        return;
      }
      createSomeCollections(1, 1, 3, useData);
      assertTrue(waitForSynchronousReplication(dbn));
      var servers = findCollectionServers(dbn, c[1].name());
      var fromServer = servers[0];
      var cinfo = arangoClusterInfoGetCollectionInfo(
          dbn, c[1].name());
      var shard = Object.keys(cinfo.shards)[0];
      assertTrue(maintenanceMode("on"));
      let result = cleanOutServer(servers[0], true);
      assertEqual(result.statusCode, 202);
      let id = JSON.parse(result.body).id;
      assertTrue(cancelAgencyJob(id));
      assertTrue(maintenanceMode("off"));
      while (queryAgencyJob(result.json.id).status === "ToDo") { // wait for job to be killed
        wait(0.1);
      }
      var job = queryAgencyJob(result.json.id);
      assertEqual(job.status, "Failed");
      assertEqual(job.abort);
      assertTrue(waitForSupervision());
      checkCollectionContents();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief kill pending moveShard job
////////////////////////////////////////////////////////////////////////////////

    testCancelPendingCleanOutServer: function () {
      if (replVersion === "2") {
        console.log("Skipping cancel test for replication 2, because move shards are not cancellable.");
        return;
      }
      if (useData) {
        for (var i = 0; i < c.length; ++i) {
          c[i].drop();
        }
        c = [];
        let otherNumDocuments = 100000;
        createSomeCollections(1, 1, 3, useData, otherNumDocuments);
        assertTrue(waitForSynchronousReplication(dbn));
        var servers = findCollectionServers(dbn, c[0].name());
        var fromServer = servers[0];
        var cinfo = arangoClusterInfoGetCollectionInfo(
            dbn, c[0].name());
        var shard = Object.keys(cinfo.shards)[0];
        let result = cleanOutServer(fromServer, true);
        assertEqual(result.statusCode, 202);
        let id = JSON.parse(result.body).id;
        while (queryAgencyJob(result.json.id).status === "ToDo") { // wait for job to start
          wait(0.1);
        }
        assertTrue(cancelAgencyJob(id));
        while (queryAgencyJob(result.json.id).status === "Pending") { // wait for job to be killed
          wait(0.1);
        }
        var job = queryAgencyJob(result.json.id);
        assertEqual(job.status, "Failed");
        assertEqual(job.abort);
        assertTrue(waitForSupervision());
        checkCollectionContents(otherNumDocuments);
      }
    },

  };
};
