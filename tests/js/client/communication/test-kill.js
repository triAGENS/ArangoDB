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
// / @author Jan Steemann
// //////////////////////////////////////////////////////////////////////////////
const _ = require('lodash');
let jsunity = require('jsunity');
let internal = require('internal');
let arangodb = require('@arangodb');
const isEnterprise = require("internal").isEnterprise();
let fs = require('fs');
let pu = require('@arangodb/testutils/process-utils');
let db = arangodb.db;

let { versionHas } = require('@arangodb/test-helper');

const isCov = versionHas('coverage');
const {
  launchSnippetInBG,
  joinBGShells,
  cleanupBGShells
} = require('@arangodb/testutils/client-tools').run;

let IM = global.instanceManager;

let waitFor = 60;
let factor = 1;
if (versionHas('asan') || versionHas('tsan') || versionHas('coverage')) {
  waitFor *= 20;
  factor = 5;
}


function KillSuite () {
  'use strict';
  // generate a random collection name
  const cn = "UnitTests" + require("@arangodb/crypto").md5(internal.genRandomAlphaNumbers(32));

  let debug = function(text) {
    console.warn(text);
  };
  let runTests = function (tests, duration) {
    assertFalse(db[cn].exists("stop"));
    let clients = [];
    debug("starting " + tests.length + " test clients");
    try {
      tests.forEach(function (test) {
        let key = test[0];
        let code = test[1];
        let client = launchSnippetInBG(IM.options, code, key, cn);
        client.done = false;
        client.failed = true; // assume the worst
        clients.push(client);
      });

      debug("running test for " + duration + " s...");

      internal.sleep(duration);

      debug("stopping all test clients");

      // broad cast stop signal
      assertFalse(db[cn].exists("stop"));
      db[cn].insert({ _key: "stop" }, { overwriteMode: "ignore" });
      joinBGShells(IM.options, clients, waitFor, cn);

      assertEqual(1 + clients.length, db[cn].count());
      let stats = {};
      clients.forEach(function (client) {
        let doc = db[cn].document(client.key);
        assertEqual(client.key, doc._key);
        assertTrue(doc.done);

        stats[client.key] = doc.iterations;
      });

      debug("test run iterations: " + JSON.stringify(stats));
    } finally {
      cleanupBGShells(clients, cn);
    }
  };

  return {

    setUp: function () {
      db._drop(cn);
      db._create(cn);

      db._drop("UnitTestsTemp");
      let c = db._create("UnitTestsTemp", { numberOfShards: 4 });
      let docs = [];
      for (let i = 0; i < 100000; ++i) {
        docs.push({ value: i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }
    },

    tearDown: function () {
      db._drop(cn);
      db._drop("UnitTestsTemp");
    },

    testKillInParallel: function () {
      let queryCode = `
let ERRORS = require('@arangodb').errors;
try {
  db._query("FOR doc IN UnitTestsTemp RETURN doc").toArray();
} catch (err) {
  if (err.errorNum !== ERRORS.ERROR_QUERY_KILLED.code) {
    /* rethrow any non-killed exception */
    throw err;
  }
}
`;

      let queryStreamCode = `
let ERRORS = require('@arangodb').errors;
try {
  db.UnitTestsTemp.toArray();
} catch (err) {
  if (err.errorNum !== ERRORS.ERROR_QUERY_KILLED.code) {
    /* rethrow any non-killed exception */
    throw err;
  }
}
`;

      let killCode = `
let queries = require("@arangodb/aql/queries");
queries.current().forEach(function(query) {
  if (checkStop(true)) {
    return false;
  } else try {
    queries.kill(query.id);

  } catch (err) { /* ignore any errors */ }
});
`;
      let tests = [
        [ 'aql-1', queryCode ],
        [ 'aql-2', queryCode ],
        [ 'aql-stream', queryStreamCode ],
        [ 'kill', killCode ],
      ];

      // run the suite for a while...
      runTests(tests, 90);
    },

    testCancelInParallel: function () {
      let queryCode = `
let result = arango.POST_RAW("/_api/cursor", {
  query: "/*test*/ FOR doc IN UnitTestsTemp RETURN doc.value",
}, { "x-arango-async": "store" });

if (result.code !== 202) {
  throw "invalid query return code: " + result.code;
}
let jobId = result.headers["x-arango-async-id"];
let id = null;
while (id === null) {
  if (checkStop(false)) {
    id = null;
    return false;
  }
  result = arango.PUT_RAW("/_api/job/" + encodeURIComponent(jobId), {});
  if (result.code === 410 || result.code === 404) {
    // killed
    break;
  } else if (result.code >= 200 && result.code <= 202) {
    id = result.parsedBody.id;
    break;
  }
  require("internal").sleep(0.1);
}
while (id !== null) {
  if (checkStop(false)) {
    return false;
  }
  result = arango.POST_RAW("/_api/cursor/" + encodeURIComponent(id), {});
  if (result.code === 410) {
    // killed
    break;
  } else if (result.code >= 200 && result.code <= 204) {
    if (!result.parsedBody.hasMore) {
      break;
    }
    id = result.parsedBody.id;
  } else {
    throw "peng! " + JSON.stringify(result.parsedBody);
  }
}
`;

      let cancelCode = `
  let result = arango.GET("/_api/job/pending");
  result.forEach(function(jobId) {
    arango.DELETE("/_api/job/" + encodeURIComponent(jobId), {}); 
  });
  require("internal").sleep(0.01 + Math.random() * 0.09);
`;
      let tests = [
        [ 'aql-1', queryCode ],
        [ 'aql-2', queryCode ],
        [ 'cancel-1', cancelCode ],
        [ 'cancel-2', cancelCode ],
      ];

      // run the suite for a while...
      runTests(tests, 30 * factor);

      // finally kill off all remaining pending jobs
      let tries = 0;
      while (tries++ < waitFor) {
        let result = arango.GET("/_api/job/pending");
        if (result.length === 0) {
          break;
        }
        result.forEach(function(jobId) {
          arango.DELETE("/_api/job/" + encodeURIComponent(jobId), {});
        });
        require("internal").sleep(1.0);
      }

      // sleep again to make sure every killed query had a chance to finish
      require("internal").sleep(3.0);
      let killed = 0;
      let queries = require("@arangodb/aql/queries").current().forEach(function(query) {
        if (!query.query.match(/\/\*test-\d+\*\//)) {
          return;
        }
        if (query.state.toLowerCase() === 'killed') {
          ++killed;
        }
      });
      if (killed > 0) {
        throw "got " + killed + " queries in state killed, but expected none!";
      }
    },

  };
}

jsunity.run(KillSuite);

return jsunity.done();
