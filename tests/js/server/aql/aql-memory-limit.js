/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, fail, AQL_EXECUTE */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for memory limits
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

const internal = require("internal");
const errors = internal.errors;
const jsunity = require("jsunity");
const db = require("@arangodb").db;

function ahuacatlMemoryLimitStaticQueriesTestSuite () {
  return {

    testUnlimited : function () {
      let actual = AQL_EXECUTE("FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)").json;
      assertEqual(100000, actual.length);
      
      actual = AQL_EXECUTE("FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", null, { memoryLimit: 0 }).json;
      assertEqual(100000, actual.length);
    },

    testLimitedButValid : function () {
      let actual = AQL_EXECUTE("FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", null, { memoryLimit: 100 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
      
      // should still be ok
      actual = AQL_EXECUTE("FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", null, { memoryLimit: 10 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
      
      // should still be ok
      actual = AQL_EXECUTE("FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", null, { memoryLimit: 5 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
      
      // should still be ok
      actual = AQL_EXECUTE("FOR i IN 1..10000 RETURN i", null, { memoryLimit: 1000 * 1000 }).json;
      assertEqual(10000, actual.length);
      
      // should still be ok
      actual = AQL_EXECUTE("FOR i IN 1..10000 RETURN i", null, { memoryLimit: 100 * 1000 + 4096 }).json;
      assertEqual(10000, actual.length);
    },

    testLimitedAndInvalid : function () {
      const queries = [
        [ "FOR i IN 1..100000 SORT CONCAT('foobarbaz', i) RETURN CONCAT('foobarbaz', i)", 200000 ],
        [ "FOR i IN 1..100000 SORT CONCAT('foobarbaz', i) RETURN CONCAT('foobarbaz', i)", 100000 ],
        [ "FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", 20000 ],
        [ "FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", 10000 ],
        [ "FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", 1000 ],
        [ "FOR i IN 1..100000 RETURN CONCAT('foobarbaz', i)", 100 ],
        [ "FOR i IN 1..10000 RETURN CONCAT('foobarbaz', i)", 10000 ],
        [ "FOR i IN 1..10000 RETURN CONCAT('foobarbaz', i)", 1000 ],
        [ "FOR i IN 1..10000 RETURN CONCAT('foobarbaz', i)", 100 ],
        [ "FOR i IN 1..10000 RETURN CONCAT('foobarbaz', i)", 10 ]
      ];

      queries.forEach(function(q) {
        try {
          AQL_EXECUTE(q[0], null, { memoryLimit: q[1] });
          fail();
        } catch (err) {
          assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
        }
      });
    },

  };
}

function ahuacatlMemoryLimitReadOnlyQueriesTestSuite () {
  const cn = "UnitTestsCollection";

  let c;

  return {
    setUpAll : function () {
      // only one shard because that is more predictable for memory usage
      c = db._create(cn, { numberOfShards: 1 });

      let docs = [];
      for (let i = 0; i < 100 * 1000; ++i) {
        docs.push({ value1: i, value2: i % 10, _key: "test" + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }
    },
    
    tearDownAll : function () {
      db._drop(cn);
    },

    testFullScan : function () {
      let actual = AQL_EXECUTE("FOR doc IN " + cn + " RETURN doc", null, { memoryLimit: 10 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
        
      try {
        AQL_EXECUTE("FOR doc IN " + cn + " RETURN doc", null, { memoryLimit: 5 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testIndexScan : function () {
      let actual = AQL_EXECUTE("FOR doc IN " + cn + " SORT doc._key RETURN doc", null, { memoryLimit: 10 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
        
      try {
        AQL_EXECUTE("FOR doc IN " + cn + " SORT doc._key RETURN doc", null, { memoryLimit: 5 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },

    testSort : function () {
      // turn off constrained heap sort
      const optimizer = { rules: ["-sort-limit"] };
      let actual = AQL_EXECUTE("FOR doc IN " + cn + " SORT doc.value1 LIMIT 10 RETURN doc", null, { memoryLimit: 15 * 1000 * 1000, optimizer }).json;
      assertEqual(10, actual.length);
        
      try {
        AQL_EXECUTE("FOR doc IN " + cn + " SORT doc.value1 LIMIT 10 RETURN doc", null, { memoryLimit: 10 * 1000 * 1000, optimizer });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testCollect1 : function () {
      let actual = AQL_EXECUTE("FOR doc IN " + cn + " COLLECT v = doc.value1 OPTIONS { method: 'hash' } RETURN v", null, { memoryLimit: 10 * 1000 * 1000 }).json;
      assertEqual(100000, actual.length);
      
      try {
        AQL_EXECUTE("FOR doc IN " + cn + " COLLECT v = doc.value1 OPTIONS { method: 'hash' } RETURN v", null, { memoryLimit: 5 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testCollect2 : function () {
      let actual = AQL_EXECUTE("FOR doc IN " + cn + " COLLECT v = doc.value2 OPTIONS { method: 'hash' } RETURN v", null, { memoryLimit: 1 * 1000 * 1000 }).json;
      assertEqual(10, actual.length);
      
      actual = AQL_EXECUTE("FOR doc IN " + cn + " COLLECT v = doc.value2 OPTIONS { method: 'hash' } RETURN v", null, { memoryLimit: 100 * 1000 }).json;
      assertEqual(10, actual.length);
        
      try {
        AQL_EXECUTE("FOR doc IN " + cn + " COLLECT v = doc.value2 OPTIONS { method: 'hash' } RETURN v", null, { memoryLimit: 10 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
  };
}

function ahuacatlMemoryLimitGraphQueriesTestSuite () {
  const vn = "UnitTestsVertex";
  const en = "UnitTestsEdge";

  return {
    setUpAll : function () {
      db._drop(en);
      db._drop(vn);
      
      const n = 400;

      // only one shard because that is more predictable for memory usage
      let c = db._create(vn, { numberOfShards: 1 });

      let docs = [];
      for (let i = 0; i <= n; ++i) {
        docs.push({ _key: "test" + i });
      }
      c.insert(docs);

      c = db._createEdgeCollection(en, { numberOfShards: 1 });

      const weight = 1;
      
      docs = [];
      for (let i = 0; i < n; ++i) {
        for (let j = i + 1; j < n; ++j) {
          docs.push({ _from: vn + "/test" + i, _to: vn + "/test" + j, weight });
          if (docs.length === 5000) {
            c.insert(docs);
            docs = [];
          }
        }
      }
      if (docs.length) {
        c.insert(docs);
      }
    },
    
    tearDownAll : function () {
      db._drop(en);
      db._drop(vn);
    },
    
    testKShortestPaths : function () {
      let actual = AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND K_SHORTEST_PATHS '" + vn + "/test0' TO '" + vn + "/test11' " + en + " RETURN p", null, { memoryLimit: 5 * 1000 * 1000 }).json;
      // no shortest path available
      assertEqual(1024, actual.length);
      
      try {
        AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND K_SHORTEST_PATHS '" + vn + "/test0' TO '" + vn + "/test11' " + en + " RETURN p", null, { memoryLimit: 1 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testKPaths : function () {
      let actual = AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND K_PATHS '" + vn + "/test0' TO '" + vn + "/test317' " + en + " RETURN p", null, { memoryLimit: 250 * 1000 }).json;
      // no shortest path available
      assertEqual(1, actual.length);
      
      try {
        AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND K_PATHS '" + vn + "/test0' TO '" + vn + "/test317' " + en + " RETURN p", null, { memoryLimit: 30 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testShortestPathDefaultWeight : function () {
      return;
      let actual = AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND SHORTEST_PATH '" + vn + "/test0' TO '" + vn + "/test310' " + en + " RETURN p", null, { memoryLimit: 1 * 1000 * 1000 }).json;
      // no shortest path available
      assertEqual(0, actual.length);
      
      try {
        AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND SHORTEST_PATH '" + vn + "/test0' TO '" + vn + "/test310' " + en + " RETURN p", null, { memoryLimit: 100 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },
    
    testShortestPathWeightAttribute : function () {
      return;
      let actual = AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND SHORTEST_PATH '" + vn + "/test0' TO '" + vn + "/test310' " + en + " RETURN p", null, { memoryLimit: 1 * 1000 * 1000, weightAttribute: "weight" }).json;
      // no shortest path available
      assertEqual(0, actual.length);
      
      try {
        AQL_EXECUTE("WITH " + vn + " FOR p IN OUTBOUND SHORTEST_PATH '" + vn + "/test0' TO '" + vn + "/test310' " + en + " RETURN p", null, { memoryLimit: 100 * 1000, weightAttribute: "weight" });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },

    testTraversal : function () {
      let actual = AQL_EXECUTE("WITH " + vn + " FOR v, e, p IN 1..2 OUTBOUND '" + vn + "/test0' " + en + " RETURN v", null, { memoryLimit: 20 * 1000 * 1000 }).json;
      assertEqual(79800, actual.length);
      
      try {
        // run query with same depth, but lower mem limit
        AQL_EXECUTE("WITH " + vn + " FOR v, e, p IN 1..4 OUTBOUND '" + vn + "/test0' " + en + " RETURN v", null, { memoryLimit: 5 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
        
      try {
        // increase traversal depth
        AQL_EXECUTE("WITH " + vn + " FOR v, e, p IN 1..5 OUTBOUND '" + vn + "/test0' " + en + " RETURN v", null, { memoryLimit: 10 * 1000 * 1000 });
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_RESOURCE_LIMIT.code, err.errorNum);
      }
    },

  };
}

jsunity.run(ahuacatlMemoryLimitStaticQueriesTestSuite);
jsunity.run(ahuacatlMemoryLimitReadOnlyQueriesTestSuite);
jsunity.run(ahuacatlMemoryLimitGraphQueriesTestSuite);

return jsunity.done();
