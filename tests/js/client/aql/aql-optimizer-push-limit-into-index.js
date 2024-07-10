/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertEqual, assertNotEqual, assertNull, assertFalse */

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
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const internal = require("internal");
const errors = internal.errors;
const db = require("@arangodb").db;
const helper = require("@arangodb/aql-helper");
const assertQueryError = helper.assertQueryError;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function optimizerPushLimitIntoIndexTestSuite () {
  let c;
  return {
    setUpAll : function () {
      db._drop("UnitTestsCollection");
      c = db._create("UnitTestsCollection"); 
      let docs = []; 
      for (i = 0; i < 1000; ++i) { 
        docs.push({ 
          _key: "test" + i, 
          date_created: "20240613" + i, 
          license: (i <= 700 ? "cc-by-sa" : (i < 900 ? "cc-by-nc" : "foo")) 
        }); 
      }
      c.insert(docs);
      c.ensureIndex({ name: "license-date", type: "persistent", fields: ["license", "date_created"] }); 
    },
    tearDownAll : function () {
      db._drop("UnitTestsCollection");
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief limit pushed into index node with applicable sorting conditions
////////////////////////////////////////////////////////////////////////////////

    testPushLimitIntoIndexRuleApplicableSortConditions : function () {
      let queries = [
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created ASC LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created DESC LIMIT 100 RETURN i._key",
      ];

      for (let i = 0; i < queries.length; ++i) {
        let plan = db._createStatement(queries[i]).explain().plan;
        let indexNode = plan.nodes[1];

        assertEqual("IndexNode", indexNode.type);
        assertEqual(indexNode.limit, 100);
        assertNotEqual(-1, plan.rules.indexOf("push-limit-into-index"));
      }
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief limit pushed into index node with inapplicable sorting conditions
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleInapplicableSortConditions : function () {
     let queries = [
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.foo LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created, i.foo LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created, i.license  LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.license ASC, i.date_created DESC LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.license ASC, i.date_created ASC LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.license DESC, i.date_created DESC LIMIT 100 RETURN i._key",
      ];
 
      for (let i = 0; i < queries.length; ++i) {
        let plan = db._createStatement(queries[i]).explain().plan;
        let indexNode = plan.nodes[1];

        assertEqual("IndexNode", indexNode.type);
        assertEqual(indexNode.limit, 0);
        assertEqual(-1, plan.rules.indexOf("push-limit-into-index"));
      }
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test case with additional conditional filtering in index node
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleConditionalFiltering : function () {
     let queries = [
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] AND i.date_created > 500 SORT i.date_created LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] and i.foo == true SORT i.date_created LIMIT 100 RETURN i._key",
      ];
 
      for (let i = 0; i < queries.length; ++i) {
        let plan = db._createStatement(queries[i]).explain().plan;
        let indexNode = plan.nodes[1];

        assertEqual("IndexNode", indexNode.type);
        assertEqual(indexNode.limit, 0);
        assertEqual(-1, plan.rules.indexOf("push-limit-into-index"));
      }
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test case with additional OR conditional filtering in index node
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleORConditionalFiltering : function () {
      let query = "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] OR i.date_created > 500 SORT i.date_created LIMIT 100 RETURN i._key";

      let plan = db._createStatement(query).explain().plan;
      let indexNodes = plan.nodes.filter(function(n) { return n.type === 'IndexNode'; });

      assertEqual(0, indexNodes.length);
      assertEqual(-1, plan.rules.indexOf("push-limit-into-index"));
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test case with double loop
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleDoubleLoop : function () {
      let query = "FOR i in 0..2 FOR j IN " + c.name() + " FILTER j.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT j.date_created LIMIT 100 RETURN j._key";

      let plan = db._createStatement(query).explain().plan;
      let indexNode = plan.nodes[3];

      assertEqual("IndexNode", indexNode.type);
      assertEqual(indexNode.limit, 0);
      assertEqual(-1, plan.rules.indexOf("push-limit-into-index"));
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test case with offset
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleWithOffset : function () {
      let query = "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created LIMIT 8, 100 RETURN i._key";

      let plan = db._createStatement(query).explain().plan;
      let indexNode = plan.nodes[1];

      assertEqual("IndexNode", indexNode.type);
      assertEqual(indexNode.limit, 108);
      assertNotEqual(-1, plan.rules.indexOf("push-limit-into-index"));
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test case when rule does not trigger
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleDoesNotTrigger: function () {
     let queries = [
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa'] SORT i.date_created DESC LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created RETURN i._key",
        "FOR i IN " + c.name() + " SORT i.date_created DESC LIMIT 100 RETURN i._key",
      ];
 
      for (let i = 0; i < queries.length; ++i) {
        let plan = db._createStatement(queries[i]).explain().plan;
        let indexNodes = plan.nodes.filter(function(n) { return n.type === 'IndexNode'; });

        assertTrue(indexNodes.length <= 1);
        if (indexNodes.length == 1) {
          assertEqual("IndexNode", indexNodes[0].type);
          assertEqual(indexNodes[0].limit, 0);
          assertEqual(-1, plan.rules.indexOf("push-limit-into-index"));
        }
      }
    },
////////////////////////////////////////////////////////////////////////////////
/// @brief test results are same with and without rule
////////////////////////////////////////////////////////////////////////////////
    testPushLimitIntoIndexRuleResults: function () {
     let queries = [
        //"FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created ASC LIMIT 100 RETURN i._key",
        "FOR i IN " + c.name() + " FILTER i.license IN ['cc-by-sa', 'cc-by-nc', 'foo'] SORT i.date_created DESC LIMIT 100 RETURN i.date_created",
      ];
 
      for (let i = 0; i < queries.length; ++i) {
        const opts = { optimizer: { rules: ["-push-limit-into-index"] } };
        let resultsWithoutRule = db._query(queries[i], null, opts).toArray();
        let resultsWithRule = db._query(queries[i]).toArray();

        assertEqual(resultsWithoutRule, resultsWithRule);
      }
    },
  };
}

jsunity.run(optimizerPushLimitIntoIndexTestSuite);

return jsunity.done();
