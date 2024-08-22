/*jshint strict: true */
/*global assertTrue, assertFalse, assertEqual, assertNotNull, print, fail*/
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
/// @author Alexandru Petenchea
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const arangodb = require("@arangodb");
const internal = require("internal");
const _ = require('lodash');
const db = arangodb.db;
const helper = require('@arangodb/test-helper');
const lh = require("@arangodb/testutils/replicated-logs-helper");
const lp = require("@arangodb/testutils/replicated-logs-predicates");
const lhttp = require('@arangodb/testutils/replicated-logs-http-helper');
const dh = require("@arangodb/testutils/document-state-helper");
const ch = require("@arangodb/testutils/collection-groups-helper");

const database = "replication2_document_store_test_db";
const collectionName = "testCollection";

function makeTestSuites(testSuite) {
  let suiteV1 = {};
  let suiteV2 = {};
  helper.deriveTestSuite(testSuite({}), suiteV1, "_V1");
  helper.deriveTestSuite(testSuite({replicationVersion: "2"}), suiteV2, "_V2");
  return [suiteV1, suiteV2];
}

/**
 * This test suite validates the correctness of most basic operations, checking replicated log entries.
 */
const replicatedStateDocumentStoreSuiteReplication2 = function () {
  let collection = null;
  let shards = null;
  let shardsToLogs = null;
  let logs = null;

  const {setUpAll, tearDownAll, setUpAnd, tearDownAnd} =
    lh.testHelperFunctions(database, {replicationVersion: "2"});

  return {
    setUpAll,
    tearDownAll,
    setUp: setUpAnd(() => {
      collection = db._create(collectionName, {"numberOfShards": 2, "writeConcern": 2, "replicationFactor": 3});
      ({shards, shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection));
    }),
    tearDown: tearDownAnd(() => {
      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testCreateReplicatedStateForEachShard: function() {
      const colPlan = lh.readAgencyValueAt(`Plan/Collections/${database}/${collection._id}`);
      const colCurrent = lh.readAgencyValueAt(`Current/Collections/${database}/${collection._id}`);

      for (const shard of collection.shards()) {
        const logId = shardsToLogs[shard];
        assertFalse(logId === undefined, `No log found for shard ${shard}`);

        let {target, plan, current} = lh.readReplicatedLogAgency(database, logId);
        let shardPlan = colPlan.shards[shard];
        let shardCurrent = colCurrent[shard];

        // Check if the replicated state in Target and the shard in Plan match
        assertEqual(target.leader, shardPlan[0]);
        assertTrue(lh.sortedArrayEqualOrError(Object.keys(target.participants)), _.sortBy(shardPlan));

        // Check if the replicated state in Plan and the shard in Plan match
        assertTrue(lh.sortedArrayEqualOrError(Object.keys(plan.participantsConfig.participants)), _.sortBy(shardPlan));

        // Check if the replicated state in Current and the shard in Current match
        assertEqual(target.leader, shardCurrent.servers[0]);
        assertTrue(lh.sortedArrayEqualOrError(Object.keys(current.localStatus)), _.sortBy(shardCurrent.servers));
      }
    },

    testReplicateOperationsCommit: function() {
      const opType = "Commit";

      dh.logIfFailure(() => {
        collection.insert({_key: "abcd"});
      }, `Failed to insert document in collection ${collectionName}`,{collections: [collection]});

      const mergedLogs = dh.mergeLogs(logs);
      let commitEntries = dh.getDocumentEntries(mergedLogs, opType);
      let insertEntries = dh.getDocumentEntries(mergedLogs, "Insert");
      assertEqual(commitEntries.length, 1,
          `Found more commitEntries than expected: ${JSON.stringify(commitEntries)}. Insert entries: ${JSON.stringify(insertEntries)}`);
      assertEqual(insertEntries.length, commitEntries.length,
          `Insert entries: ${JSON.stringify(insertEntries)} do not match Commit entries ${JSON.stringify(commitEntries)}`);
      assertEqual(insertEntries[0].trx, commitEntries[0].trx,
          `Insert entries: ${JSON.stringify(insertEntries)} do not match Commit entries ${JSON.stringify(commitEntries)}`);
    },

    testReplicateOperationsInsert: function() {
      const opType = "Insert";

      // Insert single document
      let documents = [{_key: "foo"}, {_key: "bar"}];
      dh.logIfFailure(() => {
        documents.forEach(doc => collection.insert(doc));
      }, `Failed to insert documents in collection ${collectionName}`, {collections: [collection]});
      dh.searchDocs(logs, documents, opType);

      // Insert multiple documents
      documents = [...Array(10).keys()].map(i => {return {name: "testInsert1", foobar: i};});
      dh.logIfFailure(() => {
        collection.insert(documents);
      }, `Failed to insert documents in collection ${collectionName}`, {collections: [collection]});
      let result = dh.getArrayElements(logs, opType, "testInsert1");
      for (const doc of documents) {
        assertTrue(result.find(entry => entry.foobar === doc.foobar) !== undefined);
      }

      // AQL INSERT
      documents = [...Array(10).keys()].map(i => {return {name: "testInsert2", baz: i};});
      db._query(`FOR i in 0..9 INSERT {_key: CONCAT('test', i), name: "testInsert2", baz: i} INTO ${collectionName}`);
      result = dh.getArrayElements(logs, opType, "testInsert2");
      for (const doc of documents) {
        assertTrue(result.find(entry => entry.baz === doc.baz) !== undefined);
      }
    },

    testReplicateOperationsModify: function() {
      const opType = "Update";

      // Choose two distinct random numbers
      const numDocs = 2;
      const nums = new Set();
      while(nums.size !== numDocs) {
        nums.add(_.random(1000));
      }

      // Update single document
      const documents = [...nums].map(i => ({_key: `test${i}`, value: i}));
      let docHandles = [];
      documents.forEach(doc => {
        let docUpdate = {_key: doc._key, name: `updatedTest${doc.value}`};
        let d = collection.insert(doc);
        docHandles.push(collection.update(d, docUpdate));
        dh.searchDocs(logs, [docUpdate], opType);
      });

      // Replace multiple documents
      let replacements = [{value: 10, name: "testR"}, {value: 20, name: "testR"}];
      docHandles = collection.replace(docHandles, replacements);
      let result = dh.getArrayElements(logs, "Replace",  "testR");
      for (const doc of replacements) {
        assertTrue(result.find(entry => entry.value === doc.value) !== undefined);
      }

      // Update multiple documents
      let updates = documents.map(_ => {return {name: "testModify10"};});
      docHandles = collection.update(docHandles, updates);
      result = dh.getArrayElements(logs, opType, "testModify10");
      assertEqual(result.length, updates.length);

      // AQL UPDATE
      db._query(`FOR doc IN ${collectionName} UPDATE {_key: doc._key, name: "testModify100"} IN ${collectionName}`);
      result = dh.getArrayElements(logs, opType, "testModify100");
      assertEqual(result.length, updates.length);
    },

    testReplicateOperationsRemove: function() {
      const opType = "Remove";

      // Remove single document
      let doc = {_key: `test${_.random(1000)}`};
      let d = collection.insert(doc);
      collection.remove(d);
      dh.searchDocs(logs, [doc], opType);

      // AQL REMOVE
      let documents = [
        {_key: `test${_.random(1000)}`}, {_key: `test${_.random(1000)}`}
      ];
      collection.insert(documents);
      db._query(`FOR doc IN ${collectionName} REMOVE doc IN ${collectionName}`);
      let result = dh.getArrayElements(logs, opType);
      for (const doc of documents) {
        assertTrue(result.find(entry => entry._key === doc._key) !== undefined);
      }
    },

    testReplicateOperationsTruncate: function() {
      const opType = "Truncate";
      // we need to fill the collection with some docs since truncate will only
      // use range-deletes (which are replicated as truncate op) if the number
      // of docs is > 32*1024, otherwise it uses babies removes.
      // note that our collection has multiple shards, so we need to make sure that
      // each shard has >= 32*1024 docs!
      const numberOfShards = collection.properties().numberOfShards;
      let docs = [];
      for (let j = 0; j < 1024; ++j) {
        docs.push({});
      }
      for (let i = 0; i < 33 * numberOfShards; ++i) {
        collection.insert(docs);
      }
      collection.truncate();

      let found = [];
      let allEntries = logs.reduce((previous, current) => previous.concat(current.head(1010)), []);
      for (const entry of allEntries) {
        if (entry.hasOwnProperty("payload") && entry.payload.operation.type === opType) {
          let colName = entry.payload.operation.shard;
          assertTrue(shards.includes(colName) && !found.includes(colName));
          found.push(colName);
        }
      }
      assertEqual(found.length, 2, `Dumping combined log entries (excluding inserts): ` +
          JSON.stringify(allEntries.filter(entry => !entry.hasOwnProperty("payload") ||
              entry.hasOwnProperty("payload") && entry.payload.operation.type !== "Insert"
              && entry.payload.operation.type !== "Commit")));
    },

    testFollowersAppliedIndex: function(testName) {
      for (let i = 0; i < 10; ++i) {
        collection.insert({_key: `${testName}-${i}`});
      }

      for (const log of logs) {
        lh.waitFor(() => {
          const status = log.status();
          const leader = Object.values(status.participants).find(({response:{role}}) => role === 'leader');
          const followers = Object.entries(status.participants).filter(([_,{response:{role}}]) => role === 'follower');
          const commitIndex = leader.response.local.commitIndex;
          for (const [id, follower] of followers) {
            const appliedIndex = follower.response.local.appliedIndex;
            if (appliedIndex !== commitIndex) {
              return Error(`Applied index ${appliedIndex} of follower ${id} has not reached the commit index ${commitIndex}.`);
            }
          }
          return true;
        });
      }
    },
  };
};

/**
 * This test suite intentionally triggers intermediate commit operations and validates their execution.
 */
const replicatedStateIntermediateCommitsSuite = function() {
  const rc = lh.dbservers.length;
  const servers = Object.assign({}, ...lh.dbservers.map((serverId) => ({[serverId]: lh.getServerUrl(serverId)})));
  let collection = null;
  let shards = null;
  let shardId = null;
  let logs = null;
  let shardsToLogs = null;

  const {setUpAll, tearDownAll, setUpAnd, tearDownAnd} =
    lh.testHelperFunctions(database, {replicationVersion: "2"});

  return {
    setUpAll,
    tearDownAll,
    setUp: setUpAnd(() => {
      const props = {numberOfShards: 1, writeConcern: rc, replicationFactor: rc};
      collection = db._create(collectionName, props);
      ({shards, shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection));
      shardId = shards[0];
    }),
    tearDown: tearDownAnd(() => {
      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testIntermediateCommitsNoLogEntries: function(testName) {
      db._query(`FOR i in 0..10 INSERT {_key: CONCAT('test', i), name: '${testName}', baz: i} INTO ${collectionName}`);
      let intermediateCommitEntries = dh.getDocumentEntries(dh.mergeLogs(logs), "IntermediateCommit");
      assertEqual(intermediateCommitEntries.length, 0);
    },

    testIntermediateCommitsLogEntries: function(testName) {
      db._query(`FOR i in 0..1000 INSERT {_key: CONCAT('test', i), name: '${testName}', baz: i} INTO ${collectionName}`,
        {}, {intermediateCommitCount: 1});
      let intermediateCommitEntries = dh.getDocumentEntries(dh.mergeLogs(logs), "IntermediateCommit");
      assertEqual(intermediateCommitEntries.length, 2);
    },

    testIntermediateCommitsFull: function(testName) {
      db._query(`
      FOR i in 0..2000 
      INSERT {_key: CONCAT('test', i), name: '${testName}', value: i} INTO ${collectionName}`,
        {}, {intermediateCommitCount: 1});
      let keys = [];
      for (let i = 0; i <= 2000; ++i) {
        keys.push(`test${i}`);
      }

      // Wait for the last key to be applied on all servers
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `test2000`, 2000, true);
      // Check that all keys are applied on all servers
      for (let server of Object.values(servers)) {
        let bulk = dh.getBulkDocuments(server, database, shardId, keys);
        let keysSet = new Set(keys);
        for (let doc of bulk) {
          assertTrue(keysSet.has(doc._key));
          keysSet.delete(doc._key);
        }
      }
    },

    testIntermediateCommitsPartial: function(testName) {
      // Intentionally fail the query after an intermediate commit
      try {
        db._query(`
        FOR i in 0..2000
        FILTER ASSERT(i < 1500, "was erlaube")
        INSERT {_key: CONCAT('test', i), name: '${testName}', value: i} INTO ${collectionName}`, {},
          {intermediateCommitCount: 1});
      } catch (err) {
        assertEqual(err.errorNum, internal.errors.ERROR_QUERY_USER_ASSERT.code);
      }

      // AQL operates in batches of 1000 documents. Each batch is processed in a single babies operation.
      // Intermediate commits are only performed after a babies operation.
      // The first 1000 documents are part of the first intermediate commit, after which the query fails.
      let keys = [];
      for (let i = 0; i <= 2000; ++i) {
        keys.push(`test${i}`);
      }

      // Wait for the last key to be applied on all servers
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `test999`, 999, true);

      // Check that first batch of keys is applied on all servers
      for (let server of Object.values(servers)) {
        let bulk = dh.getBulkDocuments(server, database, shardId, keys);
        for (let doc of bulk) {
          assertEqual(doc.value < 1000, doc._key !== undefined);
        }
      }
    },
  };
};

/**
 * This test suite checks the correctness of replicated operations with respect to the follower contents.
 * All tests must use a single shard, so we don't have to guess where the keys are saved.
 * writeConcern must be equal to replicationFactor, so we replicate all documents on all followers.
 */
const replicatedStateFollowerSuite = function (dbParams) {
  const rc = lh.dbservers.length;
  const isReplication2 = dbParams.replicationVersion === "2";
  const servers = Object.assign({}, ...lh.dbservers.map((serverId) => ({[serverId]: lh.getServerUrl(serverId)})));
  let collection = null;
  let shardId = null;
  let shards = null;
  let shardsToLogs = null;
  /** @type {Array<ArangoReplicatedLog>} */
  let logs = null;

  const {setUpAll, tearDownAll, setUpAnd, tearDownAnd} =
    lh.testHelperFunctions(database, dbParams);

  return {
    setUpAll,
    tearDownAll,
    setUp: setUpAnd(() => {
      collection = db._create(collectionName, {"numberOfShards": 1, "writeConcern": rc, "replicationFactor": rc});
      shards = collection.shards();
      shardId = shards[0];
      if (isReplication2) {
        shardsToLogs = lh.getShardsToLogsMapping(database, collection._id);
        logs = shards.map(shardId => db._replicatedLog(shardsToLogs[shardId]));
      } else {
        shardsToLogs = {[shardId]: null};
      }
    }),
    tearDown: tearDownAnd(() => {
      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testFollowersSingleDocument: function(testName) {
      let handle = collection.insert({_key: `${testName}-foo`, value: `${testName}-bar`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-bar`, isReplication2);

      handle = collection.update(handle, {value: `${testName}-baz`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-baz`, isReplication2);

      handle = collection.replace(handle, {_key: `${testName}-foo`, value: `${testName}-bar2`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-bar2`, isReplication2);

      collection.remove(handle);
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, null, isReplication2);
    },

    testFollowersMultiDocuments: function(testName) {
      let documents = [...Array(3).keys()].map(i => {return {_key: `${testName}-foo${i}`, value: i};});

      let handles = collection.insert(documents);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value, isReplication2);
      }

      let updates = documents.map(doc => {return {value: doc.value + 100};});
      handles = collection.update(handles, updates);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value + 100, isReplication2);
      }

      handles = collection.replace(handles, documents);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value, isReplication2);
      }

      collection.remove(handles);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, null, isReplication2);
      }
    },

    testFollowersAql: function(testName) {
      let documents = [...Array(3).keys()].map(i => {return {_key: `${testName}-foo${i}`, value: i};});

      db._query(`FOR i in 0..9 INSERT {_key: CONCAT('${testName}-foo', i), value: i} INTO ${collectionName}`);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value, isReplication2);
      }

      db._query(`FOR doc IN ${collectionName} UPDATE {_key: doc._key, value: doc.value + 100} IN ${collectionName}`);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value + 100, isReplication2);
      }

      db._query(`FOR doc IN ${collectionName} REPLACE {_key: doc._key, value: CONCAT(doc._key, "bar")} IN ${collectionName}`);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc._key + "bar", isReplication2);
      }

      db._query(`FOR doc IN ${collectionName} REMOVE doc IN ${collectionName}`);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, null, isReplication2);
      }
    },

    testFollowersTruncate: function(testName) {
      collection.insert({_key: `${testName}-foo`, value: `${testName}-bar`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-bar`, isReplication2);
      collection.truncate();
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, null, isReplication2);
    },
  };
};

/**
 * This test suite checks that everything is left clean after dropping a database.
 */
const replicatedStateDocumentStoreSuiteDatabaseDeletionReplication2 = function () {
  let previousDatabase, databaseExisted = true;
  return {
    setUpAll: function () {
      previousDatabase = db._name();
      if (!_.includes(db._databases(), database)) {
        db._createDatabase(database, {"replicationVersion": "2"});
        databaseExisted = false;
      }
      db._useDatabase(database);
    },

    tearDownAll: function () {
      db._useDatabase(previousDatabase);
      if (_.includes(db._databases(), database) && !databaseExisted) {
        db._dropDatabase(database);
      }
    },
    setUp: lh.registerAgencyTestBegin,
    tearDown: lh.registerAgencyTestEnd,

    testDropDatabase: function() {
      let collection = db._create(collectionName, {"numberOfShards": 2, "writeConcern": 2, "replicationFactor": 3});
      let shards = collection.shards();
      let shardsToLogs = lh.getShardsToLogsMapping(database, collection._id);
      db._useDatabase("_system");
      for (const shard of shards) {
        let logId = shardsToLogs[shard];
        let {plan} = lh.readReplicatedLogAgency(database, logId);
        assertFalse(plan === undefined, `Expected plan entry for shard ${logId}`);
      }
      db._dropDatabase(database);
      for (const shard of shards) {
        let logId = shardsToLogs[shard];
        let {plan} = lh.readReplicatedLogAgency(database, logId);
        assertEqual(plan, undefined, `Expected nothing in plan for replicated log ${logId}, got ${JSON.stringify(plan)}`);
      }
    },
  };
};

/**
 * This test suite checks that the cluster is still usable after we alter the state of some participants.
 * For example, after a failover.
 */
const replicatedStateRecoverySuite = function () {
  const coordinator = lh.coordinators[0];
  let collection = null;
  let extraCollections = [];
  let shards = null;
  let shardId = null;
  let logId = null;
  let log = null;
  let logs = null;
  let shardsToLogs = null;

  const {setUpAll, tearDownAll, stopServerWait, continueServerWait, setUpAnd, tearDownAnd} =
    lh.testHelperFunctions(database, {replicationVersion: "2"});

  return {
    setUpAll,
    tearDownAll,
    setUp: setUpAnd(() => {
      collection = db._create(collectionName, {numberOfShards: 1, writeConcern: 2, replicationFactor: 3});
      ({shards, shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection));
      shardId = shards[0];
      logId = shardsToLogs[shardId];
      log = logs[0];
    }),
    tearDown: tearDownAnd(() => {
      for (let col of extraCollections) {
        db._drop(col.name());
      }
      extraCollections = [];

      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testRecoveryFromFailover: function (testName) {
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      assertTrue(participants !== undefined);
      let servers = Object.assign({}, ...participants.map((serverId) => ({[serverId]: lh.getServerUrl(serverId)})));

      let handle = collection.insert({_key: `${testName}-foo`, value: `${testName}-bar`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-bar`, true);

      // Create an index. This is just to add some more entries in the log.
      let index = collection.ensureIndex({name: "katze", type: "persistent", fields: ["value"]});
      assertEqual(index.name, "katze");
      assertEqual(index.isNewlyCreated, true);

      // We unset the leader here so that once the old leader node is resumed we
      // do not move leadership back to that node.
      lh.unsetLeader(database, logId);

      // Stop the leader. This triggers a failover.
      let leader = participants[0];
      let followers = participants.slice(1);
      let term = lh.readReplicatedLogAgency(database, logId).plan.currentTerm.term;
      let newTerm = term + 2;
      stopServerWait(leader);
      lh.waitFor(lp.replicatedLogLeaderEstablished(database, logId, newTerm, followers));

      // Check if the universal abort command appears in the log during the current term.
      let logContents = lh.dumpLogHead(logId);
      let abortAllEntryFound = _.some(logContents, entry => {
        if (entry.logTerm !== newTerm || entry.payload === undefined) {
          return false;
        }
        return entry.payload.operation.type === "AbortAllOngoingTrx";
      });
      assertTrue(abortAllEntryFound, `Log contents for ${shardId}: ${JSON.stringify(logContents)}`);

      // Try a new transaction.
      servers = Object.assign({}, ...followers.map((serverId) => ({[serverId]: lh.getServerUrl(serverId)})));
      collection.update(handle, {value: `${testName}-baz`});
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-baz`, true);

      // Try an AQL query.
      let documents = [...Array(3).keys()].map(i => {return {_key: `${testName}-${i}`, value: i};});
      db._query(`FOR i in 0..3 INSERT {_key: CONCAT('${testName}-', i), value: i} INTO ${collectionName}`);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value, true);
      }

      // Resume the dead server.
      continueServerWait(leader);

      // Expect to find all values on the awakened server.
      servers = {[leader]: lh.getServerUrl(leader)};
      dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], `${testName}-foo`, `${testName}-baz`, true);
      for (let doc of documents) {
        dh.checkFollowersValue(servers, database, shardId, shardsToLogs[shardId], doc._key, doc.value, true);
      }
    },

    testRecoveryAfterCompaction: function (testName) {
      // Checks that inserted documents are still there after compaction is done.
      // Various errors that could occur during recovery are triggered intentionally (e.g. unique constraint violation).
      // Because after the leader election followers also replay the un-compacted part of the log, this test also covers
      // error handling on followers.
      let documents = [];
      for (let counter = 0; counter < 100; ++counter) {
        documents.push({_key: `${testName}-${counter}`, value: counter, temporary: counter});
      }
      for (let idx = 0; idx < documents.length; ++idx) {
        dh.logIfFailure(() => {
          collection.insert(documents[idx]);
        }, `Failed to insert document ${JSON.stringify(documents[idx])}`, {collections: [collection], logs: [log]});
      }

      // The following operation will be used to trigger errors, which should be ignored during recovery.
      const removedDoc = {_key: `${testName}-document-not-found`};
      collection.insert(removedDoc);

      // Wait for operations to be synced
      let {leader} = lh.getReplicatedLogLeaderPlan(database, logId);
      let status = log.status();
      const commitIndex = status.participants[leader].response.local.commitIndex;
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndex));

      // Trigger compaction intentionally.
      log.compact();

      // Should ignore document-not-found
      collection.remove(removedDoc);

      // Create a temporary unique index on temporary.
      let uniqueIndex = collection.ensureIndex({name: "kuh", type: "persistent", fields: ["temporary"], unique: true});
      assertEqual(uniqueIndex.name, "kuh");
      // Drop the index
      assertTrue(collection.dropIndex(uniqueIndex));
      // Wait for the index to removed from Current. This is how we know that the leader dropped the index.
      let indexId = uniqueIndex.id.split('/')[1];
      lh.waitFor(() => {
        if (dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} still in Current`);
        }
        return true;
      });

      // Insert a doc that has the same temporary as an existing one.
      // When the index creation is going to be replayed, it will fail, but the error should be ignored.
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}-temporary`, value: 1234567, temporary: 0});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      // Insert two docs that have a common property
      const commonDoc1 = {_key: `${testName}-common1`, value: 1234};
      const commonDoc2 = {_key: `${testName}-common2`, value: 1234};
      dh.logIfFailure(() => {
        collection.insert(commonDoc1);
        collection.insert(commonDoc2);
      }, `Failed to insert document`, {collections: [collection], logs: [log]});
      // Delete the first doc
      collection.remove(commonDoc1);
      // Create an unique index. When commonDoc1 insertion is replayed, it should trigger a unique constraint violation.
      // The error should be ignored during recovery.
      uniqueIndex = collection.ensureIndex({name: "eule", type: "persistent", fields: ["value"], unique: true});
      assertEqual(uniqueIndex.name, "eule");

      // Trigger leader recovery
      lh.bumpTermOfLogsAndWaitForConfirmation(database, collection);

      // Check that all keys exist on the leader.
      let checkKeys = documents.map(doc => doc._key);
      let bulk = dh.getBulkDocuments(lh.getServerUrl(leader), database, shardId, checkKeys);
      let keysSet = new Set(checkKeys);
      for (let doc of bulk) {
        assertFalse(doc.hasOwnProperty("error"), `Expected no error, got ${JSON.stringify(doc)}`);
        assertTrue(keysSet.has(doc._key));
        keysSet.delete(doc._key);
      }
    },

    testLeaderRecoverEntries: function (testName) {
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      assertTrue(participants !== undefined, `No participants found for log ${logId}`);
      let leader = participants[0];
      let followers = participants.slice(1);

      // Create another collection that is distributed like the first one.
      // This is to make sure there exists another shard within the same log.
      const extraCollectionName = `${collectionName}DistributeShardsLike`;
      const col2 = db._create(extraCollectionName, {
        numberOfShards: 1,
        distributeShardsLike: collectionName,
      });
      extraCollections.push(col2);

      // Get the commit index. We add one to it because the compaction interval is [x, y), hence we need an extra entry.
      // For example, if we were to run compaction over range [0, 6), entry 6 would not be compacted.
      let status = log.status();
      const commitIndexAfterCreateShard = status.participants[leader].response.local.commitIndex + 1;

      // Because the last entry (i.e. CreateShard) is always kept in the log, insert another one on top.
      dh.logIfFailure(() => {
        col2.insert({_key: `${testName}-a`});
      }, `Failed to insert document`, {collections: [collection, col2], logs: [log]});

      let logContents = log.head(1000);

      // We need to make sure that the CreateShard entries are being compacted before doing recovery.
      // Otherwise, the recovery procedure will try to create the shards again. The point is to test how
      // the recovery procedure handles operations referring to non-existing shards.
      // By waiting for lowestIndexToKeep to be greater than the current commitIndex, and for the index to be released,
      // we make sure the CreateShard operation is going to be compacted.
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndexAfterCreateShard));
      // Wait for all participants to have called release on the CreateShard entry.
      lh.waitFor(() => {
        log.ping();
        status = log.status();
        for (const [pid, value] of Object.entries(status.participants)) {
          if (value.response.local.releaseIndex <= commitIndexAfterCreateShard) {
            return Error(`Participant ${pid} cannot compact enough, status: ${JSON.stringify(status)}, ` +
              `log contents: ${JSON.stringify(logContents)}`);
          }
        }
        return true;
      });

      // Trigger compaction intentionally. We aim to clear a prefix of the log here, removing the CreateShard entries.
      // This way, the recovery procedure will be forced to handle entries referring to an unknown shard.
      let compactionResult = log.compact();
      for (const [pid, value] of Object.entries(compactionResult)) {
        assertEqual(value.result, "ok", `Compaction failed for participant ${pid}, result: ${JSON.stringify(value)}, ` +
          `log contents: ${JSON.stringify(logContents)}`);
      }

      // Perform some document operations on the to-be-deleted shard.
      col2.truncate();
      for (let cnt = 0; cnt < 10; ++cnt) {
        col2.insert({_key: `${testName}-${cnt}`});
        col2.replace(`${testName}-${cnt}`, {_key: `${testName}-${cnt}`, value: cnt});
      }
      col2.remove({_key: `${testName}-0`});
      col2.update({_key: `${testName}-1`}, {_key: `${testName}-1`, value: 100});

      // Modify the shard. During leader recovery, this will trigger a ModifyShard on a non-existing shard.
      col2.properties({computedValues: [{
          name: "createdAt",
          expression: "RETURN DATE_NOW()",
          overwrite: true,
          computeOn: ["insert"]
        }]});

      // Create and drop an indexes. During leader recovery, this will trigger index operations on a non-existing shard.
      let index = col2.ensureIndex({name: "eule", type: "persistent", fields: ["value"]});
      assertEqual(index.name, "eule");
      let indexId = index.id.split('/')[1];
      assertTrue(col2.dropIndex(index), `Failed to drop index ${index.name}`);
      lh.waitFor(() => {
        if (dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} still in Current`);
        }
        return true;
      });

      index = col2.ensureIndex({name: "frosch", type: "persistent", fields: ["_key"], unique: true});
      assertEqual(index.name, "frosch");
      indexId = index.id.split('/')[1];
      assertTrue(col2.dropIndex(index), `Failed to drop index ${index.name}`);
      lh.waitFor(() => {
        if (dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} still in Current`);
        }
        return true;
      });

      // Drop the additional collection. During leader recovery, this will trigger a DropShard on a non-existing shard.
      db._drop(col2.name());
      extraCollections = [];

      // Before switching to a new leader, insert some documents into the leader collection.
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}-foo1`, value: `${testName}-bar1`});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      // The following shard modification should not affect previously inserted documents.
      collection.properties({computedValues: [{
          name: "createdAt",
          expression: "RETURN DATE_NOW()",
          overwrite: true,
          computeOn: ["insert"]
        }]});
      lh.waitFor(dh.computedValuesAppliedPredicate(collection, "createdAt"));

      // The following document should have a createdAt value.
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}-foo2`, value: `${testName}-bar2`});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      logContents = log.head(1000);

      let doc = collection.document({_key: `${testName}-foo1`});
      assertFalse("createdAt" in doc, `Expected no createdAt property in ${JSON.stringify(doc)}` +
        `log contents: ${JSON.stringify(logContents)}`);
      doc = collection.document({_key: `${testName}-foo2`});
      assertTrue("createdAt" in doc, `Expected createdAt property in ${JSON.stringify(doc)}, ` +
        `log contents: ${JSON.stringify(logContents)}`);
      const createdAt = doc.createdAt;

      // Set a new leader. This will trigger recovery.
      let newLeader = followers[0];
      let term = lh.readReplicatedLogAgency(database, logId).plan.currentTerm.term;
      let newTerm = term + 1;
      lh.setLeader(database, logId, newLeader);
      lh.waitFor(lp.replicatedLogLeaderEstablished(database, logId, newTerm, _.without(participants, leader)));

      logContents = log.head(1000);

      // Check that the new leader has the correct value for the documents.
      doc = collection.document({_key: `${testName}-foo1`});
      assertFalse(doc.hasOwnProperty("createdAt"), `Expected no createdAt property in ${JSON.stringify(doc)}` +
        `log contents: ${JSON.stringify(logContents)}`);
      doc = collection.document({_key: `${testName}-foo2`});
      assertEqual(doc.createdAt, createdAt, `Expected createdAt property to be ${createdAt} in ${JSON.stringify(doc)}` +
        `log contents: ${JSON.stringify(logContents)}`);
    },

    testRecoveryMultipleCollections: function (testName) {
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      assertTrue(participants !== undefined, `No participants found for log ${logId}`);
      let leader = participants[0];
      let followers = participants.slice(1);

      // Create lots of collections that are distributed like the first one.
      // The point of this is to make sure that the recovery procedure can handle multiple shards.
      const extraCollectionName = `${collectionName}DistributeShardsLike`;
      for (let counter = 0; counter < 10; ++counter) {
        const col = db._create(`${extraCollectionName}-${counter}`, {
          numberOfShards: 1,
          distributeShardsLike: collectionName,
        });
        extraCollections.push(col);
        dh.logIfFailure(() => {
          col.insert({_key: `${testName}-${counter}`});
        }, `Failed to insert document`, {collections: [...extraCollections, collection], logs: [log]});
      }

      // Wait for operations to be synced to disk
      let status = log.status();
      const commitIndexBeforeCompaction = status.participants[leader].response.local.commitIndex;
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndexBeforeCompaction));

      // Set a new leader. This will trigger recovery.
      let newLeader = followers[0];
      let term = lh.readReplicatedLogAgency(database, logId).plan.currentTerm.term;
      let newTerm = term + 1;
      lh.setLeader(database, logId, newLeader);
      lh.waitFor(lp.replicatedLogLeaderEstablished(database, logId, newTerm, _.without(participants, leader)));

      // Check all other collections from this group.
      for (let counter = 0; counter < 10; ++counter) {
        const col = db._collection(`${extraCollectionName}-${counter}`);
        col.document({_key: `${testName}-${counter}`});
      }
    }
  };
};

/**
 * This test suite checks that replication2 leaves no side effects when creating a replication1 DB.
 */
const replicatedStateDocumentStoreSuiteReplication1 = function () {
  const dbNameR1 = "replication1TestDatabase";
  const {setUpAll, tearDownAll, setUp, tearDownAnd} =
    lh.testHelperFunctions(dbNameR1, {replicationVersion: "1"});
  let collection = null;

  return {
    setUpAll,
    tearDownAll,
    setUp,
    tearDown: tearDownAnd(() => {
      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testDoesNotCreateReplicatedStateForEachShard: function() {
      collection = db._create(collectionName, {"numberOfShards": 2, "writeConcern": 2, "replicationFactor": 3});
      let plan = lh.readAgencyValueAt(`Plan/ReplicatedLogs/${dbNameR1}`);
      assertEqual(plan, undefined, `Expected no replicated logs in agency, got ${JSON.stringify(plan)}`);
    },
  };
};

/**
 * This test suite checks the correctness of a DocumentState snapshot transfer and the related REST APIs.
 */
const replicatedStateSnapshotTransferSuite = function () {
  const coordinator = lh.coordinators[0];
  let collection = null;
  let extraCollections = [];
  let shards = null;
  let shardId = null;
  let logId = null;
  let log = null;
  let shardsToLogs = null;
  let logs = null;

  const {setUpAll, tearDownAll, setUpAnd, tearDownAnd, stopServerWait, continueServerWait} =
      lh.testHelperFunctions(database, {replicationVersion: "2"});

  const clearAllFailurePoints = () => {
    for (const server of lh.dbservers) {
      helper.debugClearFailAt(lh.getServerUrl(server));
    }
  };

  return {
    setUpAll,
    tearDownAll,
    setUp: setUpAnd(() => {
      collection = db._create(collectionName,
        {"numberOfShards": 1, "writeConcern": 2, "replicationFactor": 3});
      ({shards, shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection));
      shardId = shards[0];
      logId = shardsToLogs[shardId];
      log = logs[0];
    }),
    tearDown: tearDownAnd(() => {
      clearAllFailurePoints();

      for (let col of extraCollections) {
        db._drop(col.name());
      }
      extraCollections = [];

      if (collection !== null) {
        db._drop(collection.name());
      }
      collection = null;
    }),

    testDropCollectionOngoingTransfer: function(testName) {
      // Insert one document, so the shard is not empty.
      collection.insert({_key: testName});
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      let leaderUrl = lh.getServerUrl(participants[0]);
      const follower = participants.slice(1)[0];
      const rebootId = lh.getServerRebootId(follower);

      // This will cause the leader to hold an ongoing transaction on the shard.
      helper.debugSetFailAt(leaderUrl, "DocumentStateSnapshot::foreverReadingFromSameShard");

      let result = dh.startSnapshot(leaderUrl, database, logId, follower, rebootId);
      lh.checkRequestResult(result);

      // We should be able to drop the collection regardless of the ongoing snapshot transfer.
      db._drop(collection.name());
      collection = null;
    },

    testDropCollectionOngoingTransferDistributeShardsLike: function(testName) {
      // Create another collection that is distributed like the first one.
      let extraCollectionName = `${collectionName}-${testName}-DistributeShardsLike1`;
      const col1 = db._create(extraCollectionName, {
        distributeShardsLike: collectionName,
      });
      extraCollections.push(col1);

      extraCollectionName = `${collectionName}-${testName}-DistributeShardsLike2`;
      const col2 = db._create(extraCollectionName, {
        distributeShardsLike: collectionName,
      });
      extraCollections.push(col2);

      // Insert a document into each collection, so the shards are not empty.
      dh.logIfFailure(() => {
        collection.insert({_key: testName});
        col1.insert({_key: testName});
        col2.insert({_key: testName});
      }, `Failed to insert document`, {collections: [collection, col1, col2], logs: [log]});

      const participants = lhttp.listLogs(coordinator, database).result[logId];
      let leaderUrl = lh.getServerUrl(participants[0]);
      const follower = participants.slice(1)[0];
      const rebootId = lh.getServerRebootId(follower);

      // This will cause the leader to hold an ongoing transaction on the shard.
      helper.debugSetFailAt(leaderUrl, "DocumentStateSnapshot::foreverReadingFromSameShard");

      let result = dh.startSnapshot(leaderUrl, database, logId, follower, rebootId);
      lh.checkRequestResult(result);

      // We should be able to drop collections regardless of the ongoing snapshot transfer.
      db._drop(col1.name());
      db._drop(col2.name());
      extraCollections = [];
    },

    testFollowerSnapshotTransfer: function(testName) {
      // Prepare the grounds for replacing a follower.
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      const leader = participants[0];
      const followers = participants.slice(1);
      const oldParticipant = _.sample(followers);
      const nonParticipants = _.without(lh.dbservers, ...participants);
      assertTrue(nonParticipants.length > 0, "Not enough DBServers to run this test");
      const newParticipant = _.sample(nonParticipants);
      const newParticipants = _.union(_.without(participants, oldParticipant), [newParticipant]).sort();

      // Create another collection that is distributed like the first one.
      // This is to test the snapshot transfer of emtpy shards.
      const extraCollectionName = `${collectionName}DistributeShardsLike`;
      const col2 = db._create(extraCollectionName, {
        numberOfShards: 1,
        distributeShardsLike: collectionName,
      });
      extraCollections.push(col2);

      const numExtraCollections = 30;
      // For extra fun, create some more collections that are distributed like the first one.
      for (let counter = 0; counter < numExtraCollections; ++counter) {
        const col = db._create(`${extraCollectionName}-${counter}`, {
          numberOfShards: 1,
          distributeShardsLike: collectionName,
        });
        extraCollections.push(col);
        dh.logIfFailure(() => {
          col.insert({_key: `${testName}-${counter}`});
        }, `Failed to insert document`, {collections: [...extraCollections, collection], logs: [log]});
      }

      // Insert some documents into the original collection.
      // These documents are not going to have any computed values set.
      let documents1 = [];
      for (let counter = 0; counter < 30; ++counter) {
        documents1.push({_key: `foo-${testName}-${counter}`});
      }
      for (let idx = 0; idx < documents1.length; ++idx) {
        dh.logIfFailure(() => {
          collection.insert(documents1[idx]);
        }, `Failed to insert document ${JSON.stringify(documents1[idx])}`, {collections: [...extraCollections, collection], logs: [log]});
      }

      // Wait for operations to be synced to disk
      let status = log.status();
      const commitIndexBeforeCompaction = status.participants[leader].response.local.commitIndex;
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndexBeforeCompaction));

      let logContents = log.head(1000);

      // Trigger compaction intentionally.
      let compactionResult = log.compact();
      for (const [pid, value] of Object.entries(compactionResult)) {
        assertEqual(value.result, "ok", `Compaction failed for participant ${pid}, result: ${JSON.stringify(value)}, ` +
          `log contents: ${JSON.stringify(logContents)}`);
      }

      // After compaction, insert some extra documents. These are going to be replayed after the snapshot transfer.
      let documents2 = [];
      for (let counter = 0; counter < 30; ++counter) {
        documents2.push({_key: `bar-${testName}-${counter}`});
      }
      for (let idx = 0; idx < documents2.length; ++idx) {
        dh.logIfFailure(() => {
          collection.insert(documents2[idx]);
        }, `Failed to insert document ${JSON.stringify(documents2[idx])}`, {collections: [...extraCollections, collection], logs: [log]});
      }

      // The following shard modification should not affect previously inserted documents.
      collection.properties({computedValues: [{
          name: "createdAt",
          expression: "RETURN DATE_NOW()",
          overwrite: true,
          computeOn: ["insert", "update", "replace"]
        }]});
      lh.waitFor(dh.computedValuesAppliedPredicate(collection, "createdAt"));

      // The following documents should have a createdAt value.
      let documents3 = [];
      let createdAts = {};
      for (let counter = 0; counter < 10; ++counter) {
        documents3.push({_key: `baz'-${testName}-${counter}`});
      }
      for (let idx = 0; idx < documents3.length; ++idx) {
        let doc = collection.insert(documents3[idx], {returnNew: true});
        createdAts[doc.new._key] = doc.new.createdAt;
      }

      status = log.status();
      const commitIndexBeforeReplace = status.participants[leader].response.local.commitIndex;

      // Replace the follower.
      const result = lh.replaceParticipant(database, logId, oldParticipant, newParticipant);
      assertEqual({}, result, `Old participant ${oldParticipant} was not replaced with ${newParticipant}, log id ${logId}`);

      // Wait for replicated state to be available on the new follower.
      lh.waitFor(() => {
        const {current} = lh.readReplicatedLogAgency(database, logId);
        if (current.leader.hasOwnProperty("committedParticipantsConfig")) {
          return lh.sortedArrayEqualOrError(
            newParticipants,
            Object.keys(current.leader.committedParticipantsConfig.participants).sort());
        } else {
          return Error(`committedParticipantsConfig not found in ` +
            JSON.stringify(current.leader));
        }
      });

      // Wait for the new follower to catch up.
      const checkKeys = [...documents1.map(doc => doc._key)]
        .concat([...documents2.map(doc => doc._key)])
        .concat([...documents3.map(doc => doc._key)]);
      const newParticipantUrl = lh.getServerUrl(newParticipant);
      let bulk = [];
      lh.waitFor(() => {
        const {current} = lh.readReplicatedLogAgency(database, logId);
        const localStatus = current.localStatus[newParticipant];
        if (localStatus.snapshotAvailable === true && localStatus.spearhead.index >= commitIndexBeforeReplace) {
          bulk = dh.getBulkDocuments(newParticipantUrl, database, shardId, checkKeys);
          if (!Array.isArray(bulk)) {
            return Error(`Expected array, got ${JSON.stringify(bulk)}`);
          }
          if (bulk.length !== checkKeys.length) {
            return Error(`Expected ${checkKeys.length} documents, got ${bulk.length}`);
          }
          return true;
        }
        return Error(`Participant ${newParticipant} is not caught up, local status: ${JSON.stringify(localStatus)}`);
      });

      logContents = log.head(1000);
      let keysSet = new Set(checkKeys);
      for (let doc of bulk) {
        assertFalse(doc.hasOwnProperty("error"), `Expected no error, got ${JSON.stringify(doc)}, ` +
          `log contents: ${JSON.stringify(logContents)}`);
        assertTrue(keysSet.has(doc._key), `Expected key ${doc._key} to be in ${JSON.stringify(checkKeys)}, `
          + `log contents: ${JSON.stringify(logContents)}`);

        // During the snapshot transfer, the shard is transferred with the computed values property.
        // As the follower re-applies entries, the computed values should only appear on the documents that had them before.
        if (doc._key.startsWith("baz")) {
          assertTrue(doc.hasOwnProperty("createdAt"), `Expected createdAt property in ${JSON.stringify(doc)}, ` +
            `log contents: ${JSON.stringify(logContents)}`);
          assertEqual(doc.createdAt, createdAts[doc._key], `Expected createdAt property to be ${createdAts[doc._key]} in ${JSON.stringify(doc)}, ` +
            `log contents: ${JSON.stringify(logContents)}`);
        } else {
          assertFalse(doc.hasOwnProperty("createdAt"), `Expected no createdAt property in ${JSON.stringify(doc)}, ` +
            `log contents: ${JSON.stringify(logContents)}`);
        }
        keysSet.delete(doc._key);
      }

      // Set a newly added follower as leader.
      let term = lh.readReplicatedLogAgency(database, logId).plan.currentTerm.term;
      let newTerm = term + 1;
      lh.setLeader(database, logId, newParticipant);
      lh.waitFor(lp.replicatedLogLeaderEstablished(database, logId, newTerm, _.without(newParticipants, leader)));

      // Both collections should work as expected.
      let doc1 = collection.insert({_key: `${testName}-collection`}, {returnNew: true});
      let doc2 = col2.insert({_key: `${testName}-col2`}, {returnNew: true});
      logContents = log.head(1000);
      assertTrue(doc1.new.hasOwnProperty("createdAt"), `Expected createdAt property in ${JSON.stringify(doc1)}, ` +
        `log contents: ${JSON.stringify(logContents)}`);
      assertFalse(doc2.new.hasOwnProperty("createdAt"), `Expected no createdAt property in ${JSON.stringify(doc2)}, ` +
        `log contents: ${JSON.stringify(logContents)}`);

      // Check all other collections from this group.
      for (let counter = 0; counter < numExtraCollections; ++counter) {
        const name = `${extraCollectionName}-${counter}`;
        const key = `${testName}-${counter}`;
        const col = db._collection(name);
        try {
          col.document({_key: key});
        } catch (e) {
          print("Error: ", e);
          print("Collection contents: ", col.all().toArray());
          fail(`Expected collection ${name} to have document ${key}, but got ${e} - collection contents see above`);
        }
      }
    },

    testSnapshotDiscardedOnRebootIdChange: function() {
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      let leaderUrl = lh.getServerUrl(participants[0]);
      const follower = participants.slice(1)[0];

      // Start a new snapshot as one of the followers, and wait for the follower to be marked as failed.
      let rebootId = lh.getServerRebootId(follower);
      let result = dh.startSnapshot(leaderUrl, database, logId, follower, rebootId);
      lh.checkRequestResult(result);

      // Stop the server and wait for its rebootId to change.
      stopServerWait(follower);
      lh.waitFor(() => {
        if (lh.getServerRebootId(follower) !== rebootId) {
          return true;
        }
        return Error("follower rebootId did not change");
      });

      // The snapshot should no longer be available.
      let snapshotId = result.json.result.snapshotId;
      // We have to wait for the reboot tracker to kick in on the leader.
      lh.waitFor(() => {
        result = dh.getSnapshotStatus(leaderUrl, database, logId, snapshotId);
        if (result.json.error) {
          return true;
        }
        return Error(`Expected error, got ${JSON.stringify(result)}`);
      });

      // Pretending again to be the same follower, start a snapshot, but with a lower rebootId.
      // The expectation is that the snapshot will be discarded once the leader notices
      // the follower actually has a higher reboot ID.
      continueServerWait(follower);
      rebootId = lh.getServerRebootId(follower);
      result = dh.startSnapshot(leaderUrl, database, logId, follower, rebootId - 1);
      if (result.json.error) {
        // This is OK. The snapshot has been removed before we could return the response.
        assertEqual(result.json.errorNum, internal.errors.ERROR_INTERNAL.code, JSON.stringify(result.json));
      } else {
        // We need a waitFor here, because the snapshot cleanup is enqueued on the scheduler.
        snapshotId = result.json.result.snapshotId;
        lh.waitFor(() => {
          result = dh.getSnapshotStatus(leaderUrl, database, logId, snapshotId);
          if (result.json.error) {
            return true;
          }
          return Error(`Expected error, got ${JSON.stringify(result.json)}`);
        });
      }

      // Now start a snapshot with the correct rebootId and expect it to become available.
      rebootId = lh.getServerRebootId(follower);
      result = dh.startSnapshot(leaderUrl, database, logId, follower, rebootId);
      lh.checkRequestResult(result);
      snapshotId = result.json.result.snapshotId;
      lh.checkRequestResult(result);

      // We should be able to call /next on it
      result = dh.getNextSnapshotBatch(leaderUrl, database, logId, snapshotId);
      lh.checkRequestResult(result);

      result = dh.allSnapshotsStatus(leaderUrl, database, logId);
      lh.checkRequestResult(result);
      assertEqual(Object.keys(result.json.result.snapshots).length, 1);

      // Also, finish should work
      result = dh.finishSnapshot(leaderUrl, database, logId, snapshotId);
      lh.checkRequestResult(result);
      result = dh.getSnapshotStatus(leaderUrl, database, logId, snapshotId);
      assertTrue(result.json.error);

      result = dh.allSnapshotsStatus(leaderUrl, database, logId);
      lh.checkRequestResult(result);
      assertEqual(Object.keys(result.json.result.snapshots).length, 0);
    },

    testEffectiveWriteConcernShouldAccountForMissingSnapshots: function(testName) {
      // We start with 3 servers, so the effective write concern should be 3.
      let {plan} = lh.readReplicatedLogAgency(database, logId);
      const leader = plan.currentTerm.leader.serverId;
      const gid = plan.properties.implementation.parameters.groupId;
      lh.waitFor(lp.allServicesOperational(database, logId));

      // We need to wait for the supervision to react to services becoming operational.
      const checkEffectiveWriteConcern = (ewc) => {
        return function () {
          let {plan} = lh.readReplicatedLogAgency(database, logId);
          if (plan.participantsConfig.config.effectiveWriteConcern !== ewc) {
            return Error(`Expected effective write concern to be ${ewc}, got ${plan.participantsConfig.config.effectiveWriteConcern}`);
          }
          return true;
        };
      };

      lh.waitFor(checkEffectiveWriteConcern(3));

      // Insert a couple of documents, so there's something for the snapshot,
      // then compact the log in order to trigger a snapshot transfer next time a server is added.
      dh.logIfFailure(() => {
        for (let i = 0; i < 10; ++i) {
          collection.insert({_key: `${testName}_${i}`});
        }
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      // Wait for operations to be synced to disk
      let status = log.status();
      const commitIndexBeforeCompaction = status.participants[leader].response.local.commitIndex;
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndexBeforeCompaction));

      let logContents = log.head(1000);

      // Trigger compaction intentionally.
      let compactionResult = log.compact();
      for (const [pid, value] of Object.entries(compactionResult)) {
        assertEqual(value.result, "ok", `Compaction failed for participant ${pid}, result: ${JSON.stringify(value)}, ` +
          `log contents: ${JSON.stringify(logContents)}`);
      }

      // Leader will keep sending empty batches, so the snapshot never finishes.
      helper.debugSetFailAt(lh.getServerUrl(leader), "DocumentStateSnapshot::infiniteSnapshot");

      // Increase the replication factor to 4. This will add a new server.
      // Normally, the effective write concern should become 4, but since the snapshot transfer is stuck,
      // it should stay at 3.
      ch.modifyCollectionGroupTarget(database, gid, function (target) {
        target.attributes.mutable.replicationFactor = 4;
        target.version = 2;
      });

      // Wait for the following state to appear in Current;
      // - 3 servers are operational
      // - 1 is stuck in "AcquiringSnapshot" state
      let operational = [];
      let stuck = [];
      lh.waitFor(() => {
        let {plan, current} = lh.readReplicatedLogAgency(database, logId);
        if (plan.participantsConfig === undefined || plan.participantsConfig.participants === undefined) {
          return Error("No participants in plan");
        }

        const planParticipants = plan.participantsConfig.participants;
        if (Object.keys(planParticipants).length !== 4) {
          return Error("Not enough participants in plan");
        }

        const currentStatus = current.localStatus;
        if (currentStatus === undefined) {
          return Error("No localStatus in current");
        }
        if (Object.keys(currentStatus).length !== 4) {
          return Error("Not enough participants in current");
        }

        operational = [];
        stuck = [];
        for (let [pid, _] of Object.entries(planParticipants)) {
          if (currentStatus[pid] === undefined) {
            return Error(`No status for participant ${pid} in current`);
          }

          if (currentStatus[pid].snapshotAvailable === true && currentStatus[pid].state === "ServiceOperational") {
            operational.push(pid);
          } else if (currentStatus[pid].snapshotAvailable === false && currentStatus[pid].state === "AcquiringSnapshot") {
            stuck.push(pid);
          }
        }

        if (operational.length !== 3) {
          return Error(`Expected 3 operational servers, got ${operational}`);
        }
        if (stuck.length !== 1) {
          return Error(`Expected 1 server acquiring snapshot, got ${stuck}`);
        }

        return true;
      });

      lh.waitFor(checkEffectiveWriteConcern(3));
      // We should be able to insert documents.
      collection.insert({_key: `${testName}_foo`});

      // We even go further and stop one operational follower.
      // This leaves us with only 2 operational servers, so the effective write concern should be 2.
      const stopFollower = _.without(operational, leader)[0];
      stopServerWait(stopFollower);
      lh.waitFor(checkEffectiveWriteConcern(2));
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}_bar`});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      // Now we can finish the snapshot transfer. This should raise the effective write concern to 3.
      helper.debugClearFailAt(lh.getServerUrl(leader), "DocumentStateSnapshot::infiniteSnapshot");
      lh.waitFor(checkEffectiveWriteConcern(3));
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}_baz`});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});

      // Resuming the follower should raise the effective write concern to 4.
      continueServerWait(stopFollower);
      lh.waitFor(checkEffectiveWriteConcern(4));
      dh.logIfFailure(() => {
        collection.insert({_key: `${testName}_kuh`});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});
    }
  };
};

/**
 * This test suite checks the correctness of a DocumentState shard-related operations.
 */
const replicatedStateDocumentShardsSuite = function () {
  const coordinator = lh.coordinators[0];

  const {setUpAll, tearDownAll, setUpAnd, tearDownAnd, stopServerWait} =
      lh.testHelperFunctions(database, {replicationVersion: "2"});

  /*
   * For each shard of the collection, go through the list of corresponding servers.
   * Then, for each server, check that the shard is bound to the correct replicated log.
   */
  const checkCollectionShards = function (collection) {
    const shards = collection.shards(true);
    const shardsToLogs = lh.getShardsToLogsMapping(database, collection._id);

    for (const shard in shards) {
      const servers = shards[shard];

      // The leader must have the shard already.
      const leader = servers[0];
      let assocShards = dh.getAssociatedShards(lh.getServerUrl(leader), database, shardsToLogs[shard]);
      assertTrue(_.includes(assocShards, shard),
        `Expected shard ${shard} to be already created on leader ${leader}, but got ${JSON.stringify(assocShards)}`);

      // Followers may experience delays in creating the shard, and that is normal.
      servers.slice(1).forEach((server) => {
        lh.waitFor(() => {
          try {
            assocShards = dh.getAssociatedShards(lh.getServerUrl(server), database, shardsToLogs[shard]);
          } catch (e) {
            // The request might fail on followers in case they did not initialize the replicated state yet.
            return e;
          }
          if (_.includes(assocShards, shard)) {
            return true;
          }
          return Error(`Expected shard ${shard} to be created on server ${server}, but got ${JSON.stringify(assocShards)}`);
        });
      });
    }
  };

  /*
   * Helper for create-drop index tests. Uses the same code for unique and non-unique indexes.
   * First, the CreateIndex part is checked. Then, the index is dropped.
   */
  const indexTestHelper = function (unique, inBackground) {
    return () => {
      let collection = db._create(collectionName, {numberOfShards: 2, writeConcern: 3, replicationFactor: 3});
      let {shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection);

      // Create an index.
      let index = collection.ensureIndex({
        name: "hund",
        type: "persistent",
        fields: ["_key", "value"],
        unique: unique,
        inBackground: inBackground
      });
      assertEqual(index.name, "hund");
      assertEqual(index.isNewlyCreated, true);
      if (unique) {
        assertTrue(index.unique);
      }
      const indexId = index.id.split('/')[1];

      // Wait for the index to appear in Current. This is how we know that the leader has created it successfully.
      lh.waitFor(() => {
        if (!dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} is not in Current`);
        }
        return true;
      });

      // Check if the CreateIndex operation appears in the log.
      for (let log of logs) {
        // The CreateIndex operation may appear in the log after the index is added to Current.
        lh.waitFor(() => {
          const logContents = log.head(1000);
          if (dh.getOperationsByType(logContents, "CreateIndex").length > 0) {
            return true;
          }
          return Error(`CreateIndex not found! Contents of log ${log.id()}: ${JSON.stringify(logContents)}`);
        });
      }

      // Check that the newly created index is available on all participants.
      for (const [shard, log] of Object.entries(shardsToLogs)) {
        const participants = lhttp.listLogs(lh.coordinators[0], database).result[log];
        for (let pid of participants) {
          const serverUrl = lh.getServerUrl(pid);
          lh.waitFor(() => {
            let res = dh.getLocalIndex(serverUrl, database, `${shard}/${indexId}`);
            if (res.error) {
              return Error(`Error while getting index ${shard}/${indexId} ` +
                `from server ${pid} of log ${log}: ${JSON.stringify(res)}`);
            }
            return true;
          });

          // Check that all indexes are available (there should be two: primary and hund)
          lh.waitFor(() => {
            let res = dh.getAllLocalIndexes(serverUrl, database, shard);
            if (res.error) {
              return Error(`Error while getting all indexes from server ${pid} of log ${log}, shard ${shard}: ${JSON.stringify(res)}`);
            }
            if (res.indexes.length !== 2) {
              return Error(`Expected 2 indexes, got ${JSON.stringify(res)}`);
            }
            for (let index of res.indexes) {
              assertTrue(["primary", "hund"].includes(index.name),
                `Unexpected index name ${index.name} in ${JSON.stringify(res)}`);
            }
            return true;
          });
        }
      }

      // Drop the index.
      assertTrue(collection.dropIndex(index));

      // Wait for the index to removed from Current. This is how we know that the leader dropped the index.
      lh.waitFor(() => {
        if (dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} still in Current`);
        }
        return true;
      });

      // Check if the DropIndex operation appears in the log.
      for (let log of logs) {
        const logContents = log.head(1000);
        assertTrue(dh.getOperationsByType(logContents, "DropIndex").length > 0,
          `DropIndex not found! Contents of log ${log.id()}: ${JSON.stringify(logContents)}`);
      }

      // Check that the index is dropped on all participants.
      for (const [shard, log] of Object.entries(shardsToLogs)) {
        const participants = lhttp.listLogs(lh.coordinators[0], database).result[log];
        for (let pid of participants) {
          lh.waitFor(() => {
            let res = dh.getLocalIndex(lh.getServerUrl(pid), database, `${shard}/${indexId}`);
            if (res.error && res.code === 404) {
              return true;
            }
            return Error(`Index ${shard}/${indexId} still exists on ` +
              `server ${pid} (log ${log}): ${JSON.stringify(res)}`);
          });

          // Only the primary index should remain
          let res = dh.getAllLocalIndexes(lh.getServerUrl(pid), database, shard);
          assertFalse(res.error,
            `Error while getting all indexes from server ${pid} of log ${log}, shard ${shard}: ${JSON.stringify(res)}`);
          assertEqual(res.indexes.length, 1, `Expected 1 index, got ${JSON.stringify(res)}`);
          assertEqual(res.indexes[0].name, "primary");
        }
      }

    };
  };

  return {
    setUpAll, tearDownAll,
    setUp: setUpAnd(() => {
    }),
    tearDown: tearDownAnd(() => {
      db._drop(collectionName);
    }),

    testCreateSingleCollection: function () {
      let collection = db._create(collectionName, {numberOfShards: 4, writeConcern: 2, replicationFactor: 3});
      checkCollectionShards(collection);
    },

    testCreateMultipleCollections: function () {
      let collection = db._create(collectionName, {numberOfShards: 4, writeConcern: 2, replicationFactor: 3});
      checkCollectionShards(collection);
      try {
        const collection2 = db._create("other-collection", {
          numberOfShards: 4,
          distributeShardsLike: collectionName,
        });
        checkCollectionShards(collection2);
      } finally {
        db._drop("other-collection");
      }
    },

    testModifyShard: function () {
      const collection = db._create(collectionName, {numberOfShards: 1, writeConcern: 2, replicationFactor: 3});
      collection.properties({computedValues: [{
          name: "createdAt",
          expression: "RETURN DATE_NOW()",
          overwrite: true,
          computeOn: ["insert"]
        }]});

      // Get the log id.
      const {logId, shardId} = dh.getSingleLogId(db, collection);
      let log = db._replicatedLog(logId);

      // Wait for the shard to be modified.
      let cnt = 0;
      lh.waitFor(() => {
        ++cnt;
        dh.logIfFailure(() => {
          collection.insert({_key: `bar${cnt}`});
        }, `Failed to insert document`, {collections: [collection], logs: [log]});
        let doc = collection.document(`bar${cnt}`);
        if ("createdAt" in doc) {
          return true;
        }
        return Error(`No createdAt property in doc: ${JSON.stringify(doc)}`);
      });
      const firstTermCollection = collection.toArray();

      // Check if the ModifyShard command appears in the log during the current term.
      let logContents = log.head(1000);
      let modifyShardEntryFound = _.some(logContents, entry => {
        if (entry.payload === undefined) {
          return false;
        }
        return entry.payload.operation.type === "ModifyShard";
      });
      assertTrue(modifyShardEntryFound,
        `Log contents for shard ${shardId} (log ${logId}): ${JSON.stringify(logContents)}`);

      // Trigger a fail-over, so that ModifyShard is executed during recovery.
      const participants = lhttp.listLogs(lh.coordinators[0], database).result[logId];
      let leader = participants[0];
      let followers = participants.slice(1);
      let term = lh.readReplicatedLogAgency(database, logId).plan.currentTerm.term;
      let newTerm = term + 2;
      stopServerWait(leader);
      lh.waitFor(lp.replicatedLogLeaderEstablished(database, logId, newTerm, followers));

      // The new leader should have executed the ModifyShard command.
      dh.logIfFailure(() => {
        collection.insert({_key: "foo"});
      }, `Failed to insert document`, {collections: [collection], logs: [log]});
      let doc = collection.document("foo");
      assertTrue('createdAt' in doc, JSON.stringify(doc));

      // The previous computed-values should hold.
      for (let originalDoc of firstTermCollection) {
        let doc = collection.document(originalDoc._key);
        assertEqual(doc, originalDoc);
      }
    },

    testDoubleCreateDropIndex: function (testName) {
      let collection = db._create(collectionName, {numberOfShards: 1, writeConcern: 3, replicationFactor: 3});
      let {shards, shardsToLogs, logs} = dh.getCollectionShardsAndLogs(db, collection);
      const logId = shardsToLogs[shards[0]];
      let log = logs[0];

      // Prepare the grounds for replacing a follower.
      const participants = lhttp.listLogs(coordinator, database).result[logId];
      const leader = participants[0];
      const followers = participants.slice(1);
      const oldParticipant = _.sample(followers);
      const nonParticipants = _.without(lh.dbservers, ...participants);
      assertTrue(nonParticipants.length > 0, "Not enough DBServers to run this test");
      const newParticipant = _.sample(nonParticipants);
      const newParticipants = _.union(_.without(participants, oldParticipant), [newParticipant]).sort();

      // Create an index.
      let katze = collection.ensureIndex({
        name: "katze",
        type: "persistent",
        fields: ["foo"],
        unique: false,
        inBackground: false
      });
      assertEqual(katze.name, "katze");
      assertEqual(katze.isNewlyCreated, true);

      // Inserting docs into the collection, so there's something for the snapshot.
      for (let idx = 0; idx < 10; ++idx) {
        dh.logIfFailure(() => {
          collection.insert({_key: `${testName}-${idx}`});
        }, `Failed to insert document`, {collections: [collection], logs: [log]});
      }

      // Wait for operations to be synced to disk
      let status = log.status();
      const commitIndexBeforeCompaction = status.participants[leader].response.local.commitIndex;
      lh.waitFor(lp.lowestIndexToKeepReached(log, leader, commitIndexBeforeCompaction));

      let logContents = log.head(1000);

      // Trigger compaction intentionally.
      let compactionResult = log.compact();
      for (const [pid, value] of Object.entries(compactionResult)) {
        assertEqual(value.result, "ok", `Compaction failed for participant ${pid}, result: ${JSON.stringify(value)}, ` +
          `log contents: ${JSON.stringify(logContents)}`);
      }

      // Create another index.
      // This index will be part of the shard, but it's CreateIndex operation will also be replayed.
      let hund = collection.ensureIndex({
        name: "hund",
        type: "persistent",
        fields: ["bar"],
        unique: false,
        inBackground: false
      });
      assertEqual(hund.name, "hund");
      assertEqual(hund.isNewlyCreated, true);

      // Drop the first index.
      // The snapshot transfer will not contain the index, but it will contain the DropIndex operation though.
      assertTrue(collection.dropIndex(katze));

      // Wait for the index to removed from Current. This is how we know that the leader dropped the index.
      const indexId = katze.id.split('/')[1];
      lh.waitFor(() => {
        if (dh.isIndexInCurrent(database, collection._id, indexId)) {
          return Error(`Index ${indexId} still in Current`);
        }
        return true;
      });

      // Replace the follower.
      // This will trigger a snapshot transfer, such that the new follower executes CreateIndex and DropIndex.
      const result = lh.replaceParticipant(database, logId, oldParticipant, newParticipant);
      assertEqual({}, result, `Old participant ${oldParticipant} was not replaced with ${newParticipant}, log id ${logId}`);

      // Wait for replicated state to be available on the new follower.
      lh.waitFor(() => {
        const {current} = lh.readReplicatedLogAgency(database, logId);
        if (current.leader.hasOwnProperty("committedParticipantsConfig")) {
          return lh.sortedArrayEqualOrError(
            newParticipants,
            Object.keys(current.leader.committedParticipantsConfig.participants).sort());
        } else {
          return Error(`committedParticipantsConfig not found in ` +
            JSON.stringify(current.leader));
        }
      });
    },

    testCreateDropIndex: indexTestHelper(false, false),
    testCreateDropIndexInBackground: indexTestHelper(false, true),
    testCreateDropUniqueIndex: indexTestHelper(true, false),
    testCreateDropUniqueIndexInBackground: indexTestHelper(true, true),
  };
};


function replicatedStateFollowerSuiteV1() { return makeTestSuites(replicatedStateFollowerSuite)[0]; }
function replicatedStateFollowerSuiteV2() { return makeTestSuites(replicatedStateFollowerSuite)[1]; }

jsunity.run(replicatedStateDocumentStoreSuiteReplication2);
jsunity.run(replicatedStateDocumentStoreSuiteDatabaseDeletionReplication2);
jsunity.run(replicatedStateDocumentStoreSuiteReplication1);
jsunity.run(replicatedStateIntermediateCommitsSuite);
jsunity.run(replicatedStateFollowerSuiteV1);
jsunity.run(replicatedStateFollowerSuiteV2);
jsunity.run(replicatedStateRecoverySuite);
jsunity.run(replicatedStateSnapshotTransferSuite);
jsunity.run(replicatedStateDocumentShardsSuite);

return jsunity.done();
