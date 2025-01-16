/*global fail, assertEqual, assertNotEqual, assertTrue, assertFalse */
"use strict";

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2025 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const {db} = require("@arangodb");
const internal = require('internal');
const _ = require('lodash');

const database = "IndexCollectDatabase";
const collection = "c";
const indexCollectOptimizerRule = "use-index-for-collect";

function IndexCollectOptimizerTestSuite() {
  return {
    setUpAll: function () {
      db._createDatabase(database);
      db._useDatabase(database);

      const c = db._create(collection, {numberOfShards: 3});
      const docs = [];
      for (let k = 0; k < 10000; k++) {
        docs.push({k, a: k % 10, b: k % 100});
      }
      c.save(docs);
    },
    tearDownAll: function () {
      db._useDatabase("_system");
      db._dropDatabase(database);
    },
    tearDown: function () {
      db[collection].indexes().filter(x => x.type !== "primary").map(x => db[collection].dropIndex(x));
    },

    testSimpleDistinctScan: function () {
      db[collection].ensureIndex({type: "persistent", fields: ["a", "b", "d"]});
      db[collection].ensureIndex({type: "persistent", fields: ["k"]});

      const queries = [
        [`FOR doc IN ${collection} COLLECT a = doc.a RETURN a`, true],
        [`FOR doc IN ${collection} COLLECT b = doc.b RETURN b`, false],
        [`FOR doc IN ${collection} COLLECT k = doc.k RETURN k`, false], // because of bad selectivity
        [`FOR doc IN ${collection} COLLECT a = doc.a.c RETURN a`, false],
        [`FOR doc IN ${collection} SORT doc.a DESC COLLECT a = doc.a RETURN [a]`, false], // desc not possible
        [`FOR doc IN ${collection} SORT doc.a ASC COLLECT a = doc.a RETURN [a]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b RETURN [a, b]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b, d = doc.d RETURN [a, b, d]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a, d = doc.d RETURN [a, d]`, false],
        [`FOR doc IN ${collection} COLLECT a = doc.a, c = doc.c RETURN [a, c]`, false],
        [`FOR doc IN ${collection} COLLECT b = doc.b, a = doc.a RETURN [a, b]`, true],
        [`FOR doc IN ${collection} COLLECT c = doc.c, a = doc.a RETURN [a, c]`, false],
        [`FOR doc IN ${collection} COLLECT b = doc.b, a = doc.a OPTIONS {disableIndex: true} RETURN [a, b]`, false],
        [`FOR doc IN ${collection} COLLECT a = doc.a INTO d RETURN [a, d]`, false],
        [`FOR doc IN ${collection} COLLECT a = doc.a INTO d = doc.d RETURN [a, d]`, false],
        [`FOR doc IN ${collection} COLLECT a = doc.a WITH COUNT INTO c RETURN [a, c]`, false],
      ];

      for (const [query, optimized] of queries) {
        const explain = db._createStatement(query).explain();
        assertEqual(explain.plan.rules.indexOf(indexCollectOptimizerRule) !== -1, optimized, query);
        if (optimized) {
          const nodes = explain.plan.nodes.filter(x => x.type === "IndexCollectNode");
          assertEqual(nodes.length, 1, query);
          const indexCollect = nodes[0];
          assertEqual(indexCollect.aggregations.length, 0);
        }
      }
    },

    testSimpleAggregationScan: function () {
      db[collection].ensureIndex({type: "persistent", fields: ["a", "b", "d"], storedValues: ["e", "f"]});
      db[collection].ensureIndex({type: "persistent", fields: ["k"]});

      const queries = [
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = MAX(doc.b) RETURN [a, b]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = MAX(doc.b + doc.c) RETURN [a, b]`, false],
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = MAX(doc.b + doc.e) RETURN [a, b]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = MAX(doc.b + doc.d) RETURN [a, b]`, true],
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = MAX(doc.b + doc.d), c = PUSH(doc.f) RETURN [a, b, c]`, true],
      ];

      for (const [query, optimized] of queries) {
        const explain = db._createStatement(query).explain();
        assertEqual(explain.plan.rules.indexOf(indexCollectOptimizerRule) !== -1, optimized, query);
        if (optimized) {
          const nodes = explain.plan.nodes.filter(x => x.type === "IndexCollectNode");
          assertEqual(nodes.length, 1, query);
          const indexCollect = nodes[0];
          assertTrue(indexCollect.aggregations.length > 0);
        }
      }
    },

    testCollectOptions: function () {
      const index = db[collection].ensureIndex({type: "persistent", fields: ["a", "b", "c.d"], name: "foobar"});
      const queries = [
        [`FOR doc IN ${collection} COLLECT a = doc.a RETURN a`, "sorted", [0], [["a"]]],
        [`FOR doc IN ${collection} COLLECT a = doc.a SORT null RETURN a`, "hash", [0], [["a"]]],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b RETURN [a, b]`, "sorted", [0, 1], [["a"], ["b"]]],
        [`FOR doc IN ${collection} COLLECT b = doc.b, a = doc.a RETURN [a, b]`, "hash", [1, 0], [["b"], ["a"]]],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b, c = doc.c.d RETURN [a, b]`, "sorted", [0, 1, 2], [["a"], ["b"], ["c", "d"]]],
      ];

      for (const [query, method, expectedFields, expectedAttributes] of queries) {
        const explain = db._createStatement(query).explain();
        assertTrue(explain.plan.rules.indexOf(indexCollectOptimizerRule) !== -1, query);
        const nodes = explain.plan.nodes.filter(x => x.type === "IndexCollectNode");
        assertEqual(nodes.length, 1, query);
        const icn = nodes[0];

        assertEqual(icn.collection, collection);
        assertEqual(icn.collectOptions.method, method);
        assertEqual(icn.index.name, index.name);
        assertEqual(icn.oldIndexVariable.name, "doc");

        const indexFields = icn.groups.map(x => x.indexField);
        assertEqual(indexFields, expectedFields);

        const attributes = icn.groups.map(x => x.attribute);
        assertEqual(attributes, expectedAttributes);
      }
    }
  };
}

function IndexCollectExecutionTestSuite() {
  const numDocuments = 10000;

  return {
    setUpAll: function () {
      db._createDatabase(database);
      db._useDatabase(database);

      const c = db._create(collection, {numberOfShards: 3});
      const docs = [];
      for (let k = 0; k < numDocuments; k++) {
        docs.push({k, a: k % 2, b: k % 4, c: k % 8, m: 0});
      }
      c.save(docs);
      c.ensureIndex({type: "persistent", fields: ["k"]});
      c.ensureIndex({type: "persistent", fields: ["a", "b"], storedValues: ["c"]});
      c.ensureIndex({type: "persistent", fields: ["b"]});
      c.ensureIndex({type: "persistent", fields: ["c"]});
      c.ensureIndex({type: "persistent", fields: ["m", "a"]});
    },
    tearDownAll: function () {
      db._useDatabase("_system");
      db._dropDatabase(database);
    },

    testIndexCollectExecution: function () {

      const queries = [
        [`FOR doc IN ${collection} COLLECT a = doc.a RETURN [a]`],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b RETURN [a, b]`],
        [`FOR doc IN ${collection} COLLECT m = doc.m, a = doc.a RETURN [m, a]`],
        [`FOR doc IN ${collection} COLLECT m = doc.m RETURN [m]`],

        [`FOR doc IN ${collection} COLLECT b = doc.b, a = doc.a RETURN [a, b]`],
        [`FOR doc IN ${collection} COLLECT a = doc.a, m = doc.m RETURN [a, m]`],
        [`LET as = (FOR doc IN ${collection} COLLECT a = doc.a RETURN a) LET bs = (FOR doc IN ${collection}
            COLLECT b = doc.b RETURN b) RETURN [as, bs]`],

        [`FOR doc IN ${collection} COLLECT m = doc.m AGGREGATE a = SUM(doc.a) RETURN [m, a]`],
        [`FOR doc IN ${collection} COLLECT a = doc.a AGGREGATE b = SUM(doc.b) RETURN [a, b]`],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b AGGREGATE c = SUM(doc.c) RETURN [a, b, c]`],
        [`FOR doc IN ${collection} COLLECT a = doc.a, b = doc.b
            AGGREGATE c = BIT_XOR(doc.c), d = BIT_OR(doc.c), e = BIT_AND(doc.c) RETURN [a, b, c, d, e]`],
      ];

      for (const [query] of queries) {
        const explain = db._createStatement(query).explain();
        assertTrue(explain.plan.rules.indexOf(indexCollectOptimizerRule) !== -1, query);

        const expectedResult = db._query(query, {}, {optimizer: {rules: [`-${indexCollectOptimizerRule}`]}}).toArray();
        const actualResult = db._query(query).toArray();
        assertEqual(expectedResult, actualResult);
      }
    }
  };
}

jsunity.run(IndexCollectOptimizerTestSuite);
jsunity.run(IndexCollectExecutionTestSuite);
return jsunity.done();
