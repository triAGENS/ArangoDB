/*jshint globalstrict:false, strict:false, maxlen: 400 */
/*global fail, assertEqual, assertTrue */

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
/// @author Michael Hackstein
/// @author Copyright 2021, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const internal = require("internal");
const request = require('@arangodb/request');

const {ERROR_QUERY_COLLECTION_LOCK_FAILED, ERROR_CLUSTER_AQL_COMMUNICATION} = internal.errors;

const endpointToURL = (endpoint) => {
  if (endpoint.substr(0, 6) === 'ssl://') {
    return 'https://' + endpoint.substr(6);
  }
  const pos = endpoint.indexOf('://');
  if (pos === -1) {
    return 'http://' + endpoint;
  }
  return 'http' + endpoint.substr(pos);
};

const getEndpointById = (id) => {
  const toEndpoint = (d) => (d.endpoint);
  return global.instanceManager.arangods.filter((d) => (d.id === id))
    .map(toEndpoint)
    .map(endpointToURL)[0];
};

const callFinish = (server, route) => {
  const res = request.delete({
    url: getEndpointById(server) + "/" + route,
    body: { code: 0 }
  });
  // Engine has to be removed automatically.
  assertEqual(res.status, 404);
};


function aqlFailureSuite () {
  'use strict';
  const cn = "UnitTestsAhuacatlFailures";

  let  assertFailingQuery = function (query, error) {
    if (error === undefined) {
      error = internal.errors.ERROR_DEBUG;
    }
    try {
      db._query(query);
      fail();
    } catch (err) {
      assertEqual(error.code, err.errorNum, query);
    }
  };

  const cleanUpLeftovers = (warnings) => {
    for (const {code, message} of warnings) {
      if (code === ERROR_CLUSTER_AQL_COMMUNICATION.code) {
        const parts = message.split(" ");
        const route = parts.filter(m => m.startsWith("/_api/aql"))[0];
        // Make sure we mentioned the API route (this is dynamic to be more resilient to change).
        assertTrue(route !== undefined);
        // Make sure we mentioned the server, this is dynamic as server name is randomly generated.
        const serverInfo = parts.filter(m => m.startsWith("server:"))[0];
        assertTrue(serverInfo !== undefined);
        const serverName = serverInfo.split(":")[1];
        assertTrue(serverName !== undefined);
        callFinish(serverName, route);

        // Make sure we mentioned the database name
        const databaseName = parts.filter(m => m === "_system");
        assertEqual(databaseName.length, 1);
      }
    }
  };
        
  return {
    setUpAll: function () {
      internal.debugClearFailAt();
      db._drop(cn);
      db._create(cn, { numberOfShards: 3 });
    },

    setUp: function () {
      internal.debugClearFailAt();
    },

    tearDownAll: function () {
      internal.debugClearFailAt();
      db._drop(cn);
    },

    tearDown: function () {
      internal.debugClearFailAt();
    },

    testThatQueryIsntStuckAtShutdownIfFinishDBServerPartsThrows: function() {
      // Force cleanup to fail.
      // This should result in a positive query, but may leave locks on DBServers.
      internal.debugSetFailAt("Query::finalize_error_on_finish_db_servers");
      // NOTE we need to insert two documents here to not end up with a single remote operation node.
      const {stats, warnings} = db._query(`FOR value IN 1..2 INSERT {value} INTO ${cn}`).getExtra();
      try {
        // We force a failure upon aql/finish which produces the statistics. They are expected to be missing now.
        assertEqual(stats.writesExecuted, 0);
        // We should have at least one warning due to a failed finish call.
        assertTrue(warnings.length > 0);
      } finally {
        cleanUpLeftovers(warnings);
      }
    },

    /*
     * This test is disabled due to the following reasons:
     * If we trigger an error during the commit, the warnings are removed from the query, it is replaced by an error only.
     * Now we additionally trigger leftovers in cleanup of DBServers, which should produce warnings (which we use here to clean up ourselves)
     * As we do not have the warnings available we cannot call finish with the correct input, so this test would trigger a lock-timeout
     * which is too high to get it in for every jenkins run.
     * If we can in some other way call finish correctly this test can be activated.
    testThatQueryIsntStuckAtShutdownIfCommitAndFinishDBServerPartsThrows: function() {
      // Force commit and cleanup to fail.
      // This should result in a failed commit (error reported)
      // It should also leave locks on DBServers.
      internal.debugSetFailAt("Query::finalize_before_done");
      internal.debugSetFailAt("Query::finalize_error_on_finish_db_servers");
      assertFailingQuery(`FOR value IN 1..2 INSERT {value} INTO ${cn}`);
    },
    */
  };
}
 
if (internal.debugCanUseFailAt()) {
  jsunity.run(aqlFailureSuite);
}

return jsunity.done();
