/*jshint globalstrict:true, strict:true, esnext: true */

'use strict';

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

const db = require('@arangodb').db;
const errors = require('@arangodb').errors;
const internal = require('internal');
const jsunity = require('jsunity');
const {assertTrue, assertEqual} = jsunity.jsUnity.assertions;

const dbName = 'UnitTestsResilienceReplication2';

const lh = require('@arangodb/testutils/replicated-logs-helper');
const lp = require('@arangodb/testutils/replicated-logs-predicates');
const dh = require('@arangodb/testutils/document-state-helper');
const {
  setUpAll,
  tearDownAll,
  stopServerWait,
  stopServersWait,
  continueServerWait,
  continueServersWait,
  setUp,
  setUpAnd,
  tearDown,
  tearDownAnd,
} = lh.testHelperFunctions(dbName, {replicationVersion: '2'});

const {
  debugSetFailAt,
  debugClearFailAt,
  arangoClusterInfoFlush,
  getDBServers,
  getCtrlDBServers,
  getAgents,
  getEndpointById,
  getCoordinators,
} = require('@arangodb/test-helper');

const console = require('console');
const _ = require('lodash');

function Replication2IndexCreationResilienceSuite () {
  // Convert [{_key: "a", value: 1, ...}, ...]
  // into { "a": 1, ... }
  const docsToKV = (docs) => {
    return Object.fromEntries(
      Object.entries(_.keyBy(docs, '_key'))
        .map(([k,v]) => [k, v.value])
    );
  };

  return {
    setUpAll,
    tearDownAll,
    setUp,
    tearDown,

    testUniqueIndexReplay: function () {
      const col = db._createDocumentCollection('col',
        {numberOfShards: 1, replicationFactor: 2, writeConcern: 2});
      const {logId, shardId, log} = dh.getSingleLogId(db, col);
      const allDocsFrom = (serverId) => { return dh.getAllDocumentsFromServer(serverId, db._name(), shardId); };
      const status = log.status();
      const {leaderId} = status;
      // Fix the leader
      lh.setLeader(db._name(), logId, leaderId);
      const [followerId] = Object.keys(status.participants).filter(p => p !== leaderId);
      col.insert({_key: "a", value: 0});
      col.insert({_key: "b", value: 1});
      col.remove("col/a");
      col.insert({_key: "a", value: 1});
      col.remove("col/b");
      col.insert({_key: "b", value: 2});

      const expected = {a: 1, b: 2};
      assertEqual(expected, docsToKV(col.toArray()));
      col.ensureIndex({name: "testIndex", type: "persistent", fields: ["value"], unique: true});
      lh.waitFor(lp.allFollowersAppliedEverything(log));
      const testIndex = col.indexes().find(i => i.name === "testIndex");
      assertTrue(testIndex.unique);
      assertEqual(expected, docsToKV(col.toArray()));
      assertEqual(expected, docsToKV(allDocsFrom(leaderId)));
      assertEqual(expected, docsToKV(allDocsFrom(followerId)));
      // kind of a hack: We increase the term without running a new election.
      // This is OK in this situation, as the log should be completely settled
      // (all operations finished; writeConcern == replicationFactor; follower
      // has applied everything).
      // This way, the leader (server is unchanged) immediately runs a recovery;
      // if there was an election in between, it would have been instantiated as
      // a follower during the election-term. This should make the diagnosis of
      // possible failures in this test easier.
      lh.bumpTermOnly(db._name(), logId);
      lh.waitFor(lp.replicatedLogLeaderEstablished(db._name(), logId, undefined, []));
      assertEqual(leaderId, log.status().leaderId);
      assertEqual(expected, docsToKV(col.toArray()));
      assertEqual(expected, docsToKV(allDocsFrom(leaderId)));
      assertEqual(expected, docsToKV(allDocsFrom(followerId)));

      let term = lh.readReplicatedLogAgency(db._name(), logId).plan.currentTerm.term;
      // switching leader/follower now
      lh.setLeader(db._name(), logId, followerId);
      lh.waitFor(lp.replicatedLogIsReady(db._name(), logId, term + 1, [leaderId, followerId], followerId));
      assertEqual(expected, docsToKV(col.toArray()));
      assertEqual(expected, docsToKV(allDocsFrom(followerId))); // current leader
      assertEqual(expected, docsToKV(allDocsFrom(leaderId))); // current follower
    },
  };
}

jsunity.run(Replication2IndexCreationResilienceSuite);

return jsunity.done();
