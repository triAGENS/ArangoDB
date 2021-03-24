/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, assertTrue, assertFalse, assertEqual, assertNotEqual, arango */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
// / @author Heiko Kernbach
// //////////////////////////////////////////////////////////////////////////////

let jsunity = require('jsunity');
let internal = require('internal');
let arangodb = require('@arangodb');
let db = arangodb.db;


function QueryKillSuite() {
  'use strict';
  // generate a random collection name
  const databaseName = "UnitTestsDBTemp"
  const collectionName = "UnitTests" + require("@arangodb/crypto").md5(internal.genRandomAlphaNumbers(32));
  const docsPerWrite = 10;

  let debug = function (text) {
    console.warn(text);
  };

  const exlusiveWriteQueryString = `FOR x IN 1..${docsPerWrite} INSERT {} INTO ${collectionName} OPTIONS {exclusive: true}`;

  let executeDefaultCursorQuery = () => {
    // default execution
    try {
      db._query(exlusiveWriteQueryString);
      fail();
    } catch (e) {
      debug(e);
      assertEqual(e.errorNum, internal.errors.ERROR_QUERY_KILLED.code);
    }
  }

  let executeStreamCursorQuery = () => {
    // stream execution
    try {
      db._query(exlusiveWriteQueryString, null, null, {stream: true});
      fail();
    } catch (e) {
      debug(e);
      assertEqual(e.errorNum, internal.errors.ERROR_QUERY_KILLED.code);
    }
  }

  return {

    setUp: function () {
      db._useDatabase("_system");
      db._createDatabase(databaseName);
      db._useDatabase(databaseName);

      let collection = db._create(collectionName, {numberOfShards: 4});
      let docs = [];
      for (let i = 0; i < 100000; ++i) {
        docs.push({value: i});
        if (docs.length === 5000) {
          collection.insert(docs);
          docs = [];
        }
      }
    },

    tearDown: function () {
      db._useDatabase("_system");
      db._dropDatabase(databaseName);
    },

    testKillAfterDefaultQueryOccursInCurrent: function () { // means before query is getting registered
      // 1.) We activate the failure point ClusterQuery::afterQueryGotRegistered
      //  -> Will trigger as soon registering happens => then directly kills and continues.
      const failureName = "ClusterQuery::directKillAfterQueryGotRegistered";
      internal.debugSetFailAt(failureName);
      executeDefaultCursorQuery();
      internal.debugClearFailAt(failureName);
    },

    testKillAfterStreamQueryOccursInCurrent: function () { // means before query is getting registered
      // 1.) We activate the failure point ClusterQuery::afterQueryGotRegistered
      //  -> Will trigger as soon registering happens => then directly kills and continues.
      const failureName = "ClusterQuery::directKillAfterQueryGotRegistered";
      internal.debugSetFailAt(failureName);
      executeStreamCursorQuery();
      internal.debugClearFailAt(failureName);
    },

    testKillDefaultQueryBeforeProcessingTheQuery: function () {
      const failureName = "ClusterQuery::directKillAfterQueryIsGettingProcessed";
      internal.debugSetFailAt(failureName);
      executeDefaultCursorQuery();
      internal.debugClearFailAt(failureName);
    },

    testKillStreamQueryBeforeProcessingTheQuery: function () {
      const failureName = "ClusterQuery::directKillAfterQueryIsGettingProcessed";
      internal.debugSetFailAt(failureName);
      executeStreamCursorQuery();
      internal.debugClearFailAt(failureName);
    },

    /*
        testKillAfterQueryExecutionReturnsWaitingState: function() {},

        testKillDuringCleanup: function() {},

        testKillBeforeFinalization: function() {},

        testKillAfterFinalization: function() {}

     */
  };
}

jsunity.run(QueryKillSuite);

return jsunity.done();
