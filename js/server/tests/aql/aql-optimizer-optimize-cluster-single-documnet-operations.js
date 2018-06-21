/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertEqual, AQL_EXECUTE, AQL_EXPLAIN */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for COLLECT w/ COUNT
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var internal = require("internal");
var errors = internal.errors;
var db = require("@arangodb").db;
var helper = require("@arangodb/aql-helper");
var assertQueryError = helper.assertQueryError;
const isCluster = require("@arangodb/cluster").isCluster();

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function optimizerClusterSingleDocumentTestSuite () {
  var ruleName = "optimize-cluster-single-document-operations";
  // various choices to control the optimizer:
  var thisRuleEnabled  = { optimizer: { rules: [ "+all" ] } }; // we can only work after other rules
  var thisRuleDisabled = { optimizer: { rules: [ "+all", "-" + ruleName ] } };
  var notHereDoc = "notHereDoc";
  var yeOldeDoc = "yeOldeDoc";

  var cn1 = "UnitTestsCollection";
  var c1;

  var cn2 = "UnitTestsCollectionEmty";
  var c2;

  var cn3 = "UnitTestsCollectionModify";
  var c3;

  var s = function() {};

  var setupC1 = function() {
    db._drop(cn1);
    c1 = db._create(cn1, { numberOfShards: 5 });

    for (var i = 0; i < 20; ++i) {
      c1.save({ _key: `${i}`, group: "test" + (i % 10), value1: i, value2: i % 5 });
    }
  };

  var setupC2 = function() {
    db._drop(cn2);
    c2 = db._create(cn2, { numberOfShards: 5 });
    c2.save({_key: yeOldeDoc});
  };

  var setupC3 = function() {
    db._drop(cn3);
    c3 = db._create(cn3, { numberOfShards: 5 });

    for (var i = 0; i < 20; ++i) {
      c3.save({ _key: `${i}`, group: "test" + (i % 10), value1: i, value2: i % 5 });
    }
  };

  var pruneRevisions = function(obj) {
    if (typeof obj instanceof Array) {
      obj.forEach(function (doc) { pruneRevisions(doc);});
    } else {
      if ((obj !== null) && (typeof obj !== "string")) {
        if (obj instanceof Object) {
          if (obj.hasOwnProperty('_rev')) {
            obj._rev = "wedontcare";
          }
          for (var property in obj) {
            if (!obj.hasOwnProperty(property)) continue;
            pruneRevisions(obj[property]);
          }
        }
      }
    }
  };
  
  var explain = function (result) {
    return helper.getCompactPlan(result).map(function(node)
                                             { return node.type; });
  };
  var runTestSet = function(sets, expectedRules, expectedNodes) {
    let count = 0;

    const query = 0;
    const expectedRulesField = 1;
    const expectedNodesField = 2;
    const doFullTest = 3; // Do advanced checking
    const setupFunction = 4; // this resets the setup
    const errorCode = 5; // expected error code


    sets.forEach(function(set) {
      print(set)
      const queryString = set[query];
      const queryInfo = "count: " + count + " query info: " + JSON.stringify(set)

      var result = AQL_EXPLAIN(queryString, { }, thisRuleEnabled); // explain - only
      print(result)
      db._explain(queryString)
      assertEqual(expectedRules[set[expectedRulesField]], result.plan.rules, "rules mismatch: " + queryInfo);
      assertEqual(expectedNodes[set[expectedNodesField]], explain(result), "nodes mismatch: " + queryInfo);
      if (set[doFullTest]) {
        var r1 = {json: []}, r2 = {json: []};

        // run it first without the rule
        set[setupFunction]();
        try {
          r1 = AQL_EXECUTE(set[query], {}, thisRuleDisabled);
          assertEqual(0, set[errorCode], "we have no error in the original, but the tests expects an exception: " + queryInfo);
        }
        catch (y) {
          assertTrue(set[errorCode].hasOwnProperty('code'), "original plan throws, but we don't expect an exception" + JSON.stringify(y) + queryInfo);
          assertEqual(y.errorNum, set[errorCode].code, "match other error code - got: " + JSON.stringify(y) + queryInfo);
        }

        // Run it again with our rule
        set[setupFunction]();
        try {
          r2 = AQL_EXECUTE(set[query], {}, thisRuleEnabled);
          assertEqual(0, set[errorCode], "we have no error in our plan, but the tests expects an exception" + queryInfo);
        }
        catch (x) {
          assertTrue(set[errorCode].hasOwnProperty('code'), "our plan throws, but we don't expect an exception" + JSON.stringify(x) + queryInfo);
          assertEqual(x.errorNum, set[errorCode].code, "match our error code" + JSON.stringify(x) + queryInfo);
        }
        pruneRevisions(r1);
        pruneRevisions(r2);
        assertEqual(r1.json, r2.json, set);
      }
      count += 1;
    });
  };

  return {
    setUp : function () {
      setupC1();
      setupC2();
      setupC3();
    },

    tearDown : function () {
      db._drop(cn1);
    },

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief test plans that should result
    ////////////////////////////////////////////////////////////////////////////////

    testRuleFetch : function () {
      var queries = [
        [ "FOR d IN " + cn1 + " FILTER d._key == '1' RETURN d", 0, 0, true, s, 0],
        [ "FOR d IN " + cn1 + " FILTER d.xyz == '1' RETURN d", 1, 1, false, s, 0],

      ];
      var expectedRules = [[ "use-indexes",
                             "remove-filter-covered-by-index",
                             "remove-unnecessary-calculations-2",
                             "optimize-cluster-single-document-operations" ],
                           [ "scatter-in-cluster",
                             "distribute-filtercalc-to-cluster",
                             "remove-unnecessary-remote-scatter" ]
                          ];

      var expectedNodes = [
        [ "SingletonNode", "SingleRemoteOperationNode", "ReturnNode" ],
        [ "SingletonNode", "EnumerateCollectionNode", "CalculationNode",
          "FilterNode", "RemoteNode", "GatherNode", "ReturnNode"  ]
      ];
      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleInsert : function () {
      var queries = [
        // [ query, expectedRulesField, expectedNodesField, doFullTest, setupFunction, errorCode ]
        [ `INSERT {_key: '${notHereDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, ignoreErrors:true}`, 0, 0, true, setupC2, 0 ],
        [ `INSERT {_key: '${yeOldeDoc}',  insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, ignoreErrors:true}`, 0, 0, true, setupC2, 0 ],

        [ `INSERT {_key: '${notHereDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN OLD`, 0, 1, true, setupC2, 0 ],
        [ `INSERT {_key: '${yeOldeDoc}',  insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN OLD`, 0, 1, true, setupC2, 0 ],

        [ `INSERT {_key: '${notHereDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN NEW`, 0, 1, true, setupC2, 0 ],
        [ `INSERT {_key: '${yeOldeDoc}',  insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN NEW`, 0, 1, true, setupC2, 0 ],

        [ `INSERT {_key: '${notHereDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: false} RETURN NEW`, 0, 1, true, setupC2, 0 ],
        [ `INSERT {_key: '${yeOldeDoc}',  insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: false} RETURN NEW`, 0, 1, true, setupC2, errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED],

        [ `INSERT {_key: '${yeOldeDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN [OLD, NEW]`, 1, 2, true, setupC2, 0 ],
        [ `INSERT {_key: '${yeOldeDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN { old: OLD, new: NEW }`, 1, 2, true, setupC2, 0 ]
      ];

      var expectedRules = [
        [ "remove-data-modification-out-variables",
          "optimize-cluster-single-document-operations"
        ],
        [  "optimize-cluster-single-document-operations" ]
      ];
      var expectedNodes = [
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", 
          "ReturnNode" ]
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleUpdate : function () {

      var queries = [
        [ "UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        [ "UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        [ "UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        [ "UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        [ "UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        [ "UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        [ "UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 5, 2, false],
        [ "UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 5, 2, false],
        [ "UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 5, 2, false],
        [ "UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 5, 2, false],
        [ "UPDATE {_key: '1', boom: true } INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 5, 2, true, setupC1, 0],          
        [ "UPDATE {_key: '1'} WITH {foo: 'bar1a'} IN " + cn1 + " OPTIONS {}", 1, 0, true, s, 0],
        [ "UPDATE {_key: '1'} WITH {foo: 'bar2a'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        [ "UPDATE {_key: '1'} WITH {foo: 'bar3a'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, s, 0],
        [ "UPDATE {_key: '1'} WITH {foo: 'bar4a'} IN " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 4, 2, true, setupC1, 0],        
        [ "UPDATE {_key: '1'} WITH {foo: 'bar5a'} IN " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 4, 2, true, setupC1, 0],
        [ `FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc INTO ${cn1} OPTIONS {} RETURN NEW`, 6, 3, true, s, 0],
        [ `FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW]`, 7, 2, true, setupC1, 0],
      ];

      var expectedRules = [
        [ "remove-data-modification-out-variables", "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables",
          "optimize-cluster-single-document-operations"],
        [ "move-calculations-up", "move-calculations-up-2",
          "remove-data-modification-out-variables", "optimize-cluster-single-document-operations" ],
        [   "move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", 
            "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "move-calculations-up-2", "optimize-cluster-single-document-operations" ],
        [ "optimize-cluster-single-document-operations" ],
        [ "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", 
          "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", 
          "optimize-cluster-single-document-operations" ]

      ];

      var expectedNodes = [
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        [ "SingletonNode", "SingleRemoteOperationNode", "ReturnNode" ]
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleReplace : function () {

      var queries = [
        [ "REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        [ "REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        [ "REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        [ "REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        [ "REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        [ "REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        [ "REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 5, 2, false],
        [ "REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 5, 2, false],
        [ "REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 5, 2, false],
        [ "REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 5, 2, false],          
        [ "REPLACE {_key: '1'} WITH {foo: 'bar1a'} IN " + cn1 + " OPTIONS {}", 1, 0, true, s, 0],
        [ "REPLACE {_key: '1'} WITH {foo: 'bar2a'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        [ "REPLACE {_key: '1'} WITH {foo: 'bar3a'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, s, 0],
        [ "REPLACE {_key: '1'} WITH {foo: 'bar4a'} IN " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 4, 2, true, setupC1, 0],   
        [ "REPLACE {_key: '1', boom: true } IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 5, 2, true, setupC1, 0],
        [ "REPLACE {_key: '1'} WITH {foo: 'bar5a'} IN " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 4, 2, true, setupC1, 0],
        [ `FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW]`, 7, 2, true, setupC1, 0],
        [ `FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE doc INTO ${cn1} OPTIONS {} RETURN NEW`, 6, 3, true, setupC1, 0],
      ];

      var expectedRules = [
        [ "remove-data-modification-out-variables", "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables",
          "optimize-cluster-single-document-operations"],
        [ "move-calculations-up", "move-calculations-up-2",
          "remove-data-modification-out-variables", "optimize-cluster-single-document-operations" ],
        [   "move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", 
            "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "move-calculations-up-2", "optimize-cluster-single-document-operations" ],
        [ "optimize-cluster-single-document-operations" ],
        [ "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", 
          "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations" ],
        [ "move-calculations-up", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", 
          "optimize-cluster-single-document-operations" ]
      ];

      var expectedNodes = [
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode" ],
        [ "SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        [ "SingletonNode", "SingleRemoteOperationNode", "ReturnNode" ]
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },
    
    testRuleRemove : function () {
      var queries = [

        [ "REMOVE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, setupC1, 0],
        [ "REMOVE {_key: '2'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, setupC1, 0],
        [ "REMOVE {_key: '3'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, setupC1, 0],
        [ "REMOVE {_key: '4'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, setupC1, 0],
        [ `FOR doc IN ${cn1} FILTER doc._key == '1' REMOVE doc IN ${cn1} RETURN OLD`, 1, 1, true, setupC1, 0],
      ];
      var expectedRules = [
        ["remove-data-modification-out-variables", "optimize-cluster-single-document-operations" ],
        [ "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", 
          "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations" ]
     ];

      var expectedNodes = [
        ["SingletonNode",
         "SingleRemoteOperationNode" ],
        [ "SingletonNode",
          "SingleRemoteOperationNode",
          "ReturnNode" ],
      ];
      runTestSet(queries, expectedRules, expectedNodes);
    }
  };
}
jsunity.run(optimizerClusterSingleDocumentTestSuite);

return jsunity.done();
