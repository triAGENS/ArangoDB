/*jshint globalstrict:false, strict:false */
/*global fail, assertEqual, assertNotEqual, assertTrue, assertFalse, assertNull */

////////////////////////////////////////////////////////////////////////////////
/// @brief test index methods
///
/// DISCLAIMER
///
/// Copyright 2024 ArangoDB GmbH
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const internal = require("internal");
const arango = internal.arango;
const sleep = require('internal').sleep;
const { getDBServerEndpoints } = require("@arangodb/test-helper");
const isCluster = internal.isCluster();
let { instanceRole } = require('@arangodb/testutils/instance');
let IM = global.instanceManager;

const filter = isCluster ? instanceRole.dbServer: instanceRole.single;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite: basics
////////////////////////////////////////////////////////////////////////////////

function IndexSuite() {
  'use strict';

  let cn = "c0l";
  let c;

  return {

    setUp: function() {
      c = internal.db._create(cn, {numberOfShards: 3, replicationFactor: 2});
      let docs = [];
      for (let j = 0; j < 16; ++j) {
        docs= [];
        for (let i = 0; i < 10240; ++i) {
          docs.push({name : "name" + i});
        }
        c.insert(docs);
      }
    },

    tearDown: function() {
      IM.debugRemoveFailAt("fillIndex::pause", filter);
      IM.debugRemoveFailAt("fillIndex::unpause", filter);
      c.drop();
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test: indexes
////////////////////////////////////////////////////////////////////////////////

    testIndexProgressIsReported : function () {
      IM.debugSetFailAt("fillIndex::pause", filter);
      let idxdesc = { name: "progress", type: "persistent", fields: ["name"], inBackground: true};

      let job = arango.POST_RAW(`/_api/index?collection=${cn}`, idxdesc,
                                {"x-arango-async": "store"}).headers["x-arango-async-id"];
      let count = 0;
      let progress = 0.0;
      let idxs = [];
      while (true) {
        // Wait until the index is there (with withHidden):
        idxs = arango.GET(`/_api/index?collection=${cn}&withHidden=true`);
        assertTrue(idxs.hasOwnProperty("indexes"), idxs);
        idxs = idxs.indexes;
        if (idxs.length > 1) {
          assertEqual(idxs[1].name, "progress", idxs);
          break;
        }
        sleep(0.1);
        if (++count > 1000) {
          assertFalse(true, "Did not see hidden index in time!");
        }
      }
      assertEqual(2, idxs.length, idxs);

      let seenProgress = false;
      count = 0;
      while (true) {
        idxs = arango.GET(`/_api/index?collection=${cn}&withHidden=true`);
        assertTrue(idxs.hasOwnProperty("indexes"), idxs);
        idxs = idxs.indexes;
        assertEqual(2, idxs.length, idxs);
        let idx = idxs[1];
        assertEqual(idx.name, "progress", idx); // Check we have the right one!
        if (idx.hasOwnProperty("progress")) {
          assertTrue(idx.progress >= progress, {idx, progress});
          assertTrue(idx.isBuilding, idx);
          progress = idx.progress;
          if (progress > 0 && !seenProgress) {
            // Only release index building once we have seen at least
            // once an isBuilding state with non-zero progress
            IM.debugSetFailAt("fillIndex::unpause", filter);
            seenProgress = true;
          }
        } else if (seenProgress) {
          assertFalse(idx.hasOwnProperty("isBuilding"));
          break;
        }
          
        sleep(0.1);
        if (++count > 4000) {
          // Value intentionally high for ASAN runs, this is 100x slower
          // than observed on an old machine with debug build!
          assertFalse(true, "Did not see index finished in time! " + JSON.stringify(idx));
        }
      }
      assertTrue(seenProgress, "Never saw progress being reported!");
    },
    
    testIndexProgressIsNotReported : function () {
      IM.debugSetFailAt("fillIndex::pause", filter);
      let idxdesc = { name: "progress", type: "persistent", fields: ["name"], inBackground: true};

      let job = arango.POST_RAW(`/_api/index?collection=${cn}`, idxdesc,
                                {"x-arango-async": "store"}).headers["x-arango-async-id"];
      let count = 0;
      // check for 5 seconds that the in-progress index does not appear in the result.
      // using 5 elapsed seconds as the test's stop condition is not a guarantee that
      // the in-flight index won't appear later, but we do not have a better stop
      // condition for the test.
      while (true) {
        // do not use withHidden here!
        let idxs = arango.GET(`/_api/index?collection=${cn}`).indexes;
        assertTrue(1, idxs.length, idxs);
        assertEqual("primary", idxs[0].type);
        sleep(0.1);
        if (++count > 50) {
          break;
        }
      }
            
      IM.debugSetFailAt("fillIndex::unpause", filter);
      
      count = 0;
      // wait until index appears, at most 30 seconds
      while (++count < 3000) {
        let idxs = arango.GET(`/_api/index?collection=${cn}`).indexes;
        assertEqual("primary", idxs[0].type);
        if (idxs.length === 2) {
          assertEqual("progress", idxs[1].name);
          break;
        }
        sleep(0.1);
      }
      assertTrue(count < 3000, count);
    },
  };
}

jsunity.run(IndexSuite);
return jsunity.done();
