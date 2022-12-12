/*jshint globalstrict:false, strict:false */
/*global assertEqual, assertNull, fail */

////////////////////////////////////////////////////////////////////////////////
/// @brief test the collection interface
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
/// @author Dr. Frank Celler
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");

var arangodb = require("@arangodb");
var internal = require("internal");

var ArangoCollection = arangodb.ArangoCollection;
var db = arangodb.db;
var ERRORS = arangodb.errors;

function CollectionSuite() {
  let cn = "example";
  return {
    setUp: function() {
      db._drop(cn);
    },

    tearDown: function() {
      db._drop(cn);
    },

    testCreateWithInvalidIndexes1 : function () {
      // invalid indexes will be rejected
      try {
        db._create(cn, { indexes: [{ id: "1", type: "edge", fields: ["_from"] }] });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },
    
    testCreateWithInvalidIndexes2 : function () {
      // invalid indexes will be rejected
      try {
        db._create(cn, { indexes: [{ id: "1234", type: "hash", fields: ["a"] }] });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },

    testTruncateSelectivityEstimates : function () {
      let c = db._create(cn);
      c.ensureIndex({ type: "hash", fields: ["value"] });
      c.ensureIndex({ type: "skiplist", fields: ["value2"] });

      // add enough docs to trigger RangeDelete in truncate
      const docs = [];
      for (let i = 0; i < 35000; ++i) {
        docs.push({value: i % 250, value2: i % 100});
      }
      c.save(docs); 

      c.truncate({ compact: false });
      assertEqual(c.count(), 0);
      assertEqual(c.all().toArray().length, 0);

      // Test Selectivity Estimates
      {
        internal.waitForEstimatorSync(); // make sure estimates are consistent
        let indexes = c.getIndexes(true);
        for (let i of indexes) {
          switch (i.type) {
            case 'primary':
              assertEqual(i.selectivityEstimate, 1);
              break;
            case 'hash':
              assertEqual(i.selectivityEstimate, 1);
              break;
            case 'skiplist':
              assertEqual(i.selectivityEstimate, 1);
              break;
            default:
              fail();
          }
        }
      }
    }

  };
}

jsunity.run(CollectionSuite);

return jsunity.done();
