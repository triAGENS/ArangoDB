/*jshint globalstrict:false, strict:false */
/*global assertEqual, assertNotEqual, assertTrue */

////////////////////////////////////////////////////////////////////////////////
///
/// Copyright 2023 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License")
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
/// @author Valery Mironov
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

'use strict';
const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const { getCompleteMetricsValues } = require('@arangodb/test-helper');

const isWindows = (require("internal").platform.substr(0, 3) === 'win');
  
function MemoryMetrics() {
  const collectionName = "test";
  const viewName = "testView";

  let generateAndInsert = (collName) => {
    if( typeof generateAndInsert.counter == 'undefined' ) {
      generateAndInsert.counter = 0;
    }
    if( typeof generateAndInsert.factor == 'undefined' ) {
      generateAndInsert.factor = 0;
    }
    generateAndInsert.factor++;

    let docs = [];
    for (let i = 0; i < 1000 * generateAndInsert.factor; i++) {
      let custom_field = "field_" + generateAndInsert.counter;
      let d = {
        'stringValue': "" + generateAndInsert.counter, 
        'numericValue': generateAndInsert.counter
      };
      d[custom_field] = generateAndInsert.counter;
      docs.push(d);
      generateAndInsert.counter++
    }
    db._collection(collName).save(docs);
  };

  return {
    setUpAll: function () {
      db._create(collectionName, {replicationFactor:3, writeConcern:3, numberOfShards : 3});
      generateAndInsert(collectionName)
      db._createView(viewName, "arangosearch", {
      links: {[collectionName]: {
          includeAllFields: true,
          primarySort: {"fields": [{ "field": "stringValue", "desc": true }]},
          storedValues: ["numericValue"],
          consolidationIntervalMsec: 100,
          commitIntervalMsec: 100,
        }}
      });
      const writers = getCompleteMetricsValues("arangodb_search_writers_memory_usage");
      assertTrue(writers > 0);
      const descriptors = getCompleteMetricsValues("arangodb_search_file_descriptors");
      assertTrue(descriptors > 0);
    },
    
    tearDownAll: function () {
      db._dropView(viewName);
      db._drop(collectionName);
    },

    testSimple: function () {

      for (let i = 0; i < 5; i++) {
        let writersOldest = getCompleteMetricsValues("arangodb_search_writers_memory_usage");
        generateAndInsert(collectionName)
        db._query(`for d in ${viewName} OPTIONS {waitForSync: true} LIMIT 1 RETURN 1 `);
        let writersOlder = getCompleteMetricsValues("arangodb_search_writers_memory_usage");
        assertNotEqual(writersOldest, writersOlder);
      }

      let readers, consolidations, descriptors, mapped, scheduler_memory, num_worker_threads;
      for (let i = 0; i < 100; ++i) {

        generateAndInsert(collectionName)
        db._query(`for d in ${viewName} SEARCH d.numericValue == 100 RETURN d`);

        [readers, consolidations, descriptors, mapped, scheduler_memory, num_worker_threads] = getCompleteMetricsValues([
          "arangodb_search_readers_memory_usage", 
          "arangodb_search_consolidations_memory", 
          "arangodb_search_file_descriptors",
          "arangodb_search_mapped_memory",
          "arangodb_scheduler_stack_memory_usage",
          "arangodb_scheduler_num_worker_threads"
        ]);

        assertEqual(scheduler_memory, num_worker_threads * 4 * 1000000, {scheduler_memory, num_worker_threads});

        if (readers > 0 && consolidations > 0 && descriptors > 0 && (isWindows || mapped > 0)) {
          break;
        }
      }

      assertTrue(readers > 0);
      assertTrue(consolidations > 0);
      if (!isWindows) {
        assertTrue(mapped > 0);
      }

      {
        const oldValue = getCompleteMetricsValues("arangodb_search_writers_memory_usage");
        db._query("FOR d IN " + collectionName + " REMOVE d IN " + collectionName);
        const newValue = getCompleteMetricsValues("arangodb_search_writers_memory_usage");
        assertNotEqual(oldValue, newValue);
      }

      db._query(`for d in ${viewName} OPTIONS {waitForSync: true} LIMIT 1 RETURN 1 `);
      [readers, consolidations, descriptors, mapped, scheduler_memory, num_worker_threads] = getCompleteMetricsValues([
        "arangodb_search_readers_memory_usage", 
        "arangodb_search_consolidations_memory",
        "arangodb_search_file_descriptors",
        "arangodb_search_mapped_memory",
        "arangodb_scheduler_stack_memory_usage",
        "arangodb_scheduler_num_worker_threads"
      ]);

      assertEqual(scheduler_memory, num_worker_threads * 4 * 1000000, {scheduler_memory, num_worker_threads});

      assertTrue(readers == 0);
      assertTrue(consolidations == 0);
      assertTrue(descriptors == 0);
      assertTrue(mapped == 0);
    }
  };
}

jsunity.run(MemoryMetrics);

return jsunity.done();
