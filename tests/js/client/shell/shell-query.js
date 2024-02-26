/*jshint globalstrict:false, strict:false, maxlen:1000*/
/*global assertEqual, fail */

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
/// @author Simran Spiller
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");

const arangodb = require("@arangodb");
const db = arangodb.db;
const ERRORS = arangodb.errors;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite: _query function
////////////////////////////////////////////////////////////////////////////////

function QuerySuite () {
  'use strict';
  return {

////////////////////////////////////////////////////////////////////////////////
/// @brief test _query function with options
////////////////////////////////////////////////////////////////////////////////

    testQueryOptionsTtlError : function () {
      var result = db._query("FOR i IN 1..2000 RETURN i", { }, {
        ttl : 0.00001,
        batchSize : 1000, // default
      });
      var docs = [ ];
      try {
        while (result.hasNext()) {
          docs.push(result.next());
        }
        result = true;
        fail();
      } catch (e) {
        assertEqual(ERRORS.ERROR_CURSOR_NOT_FOUND.code, e.errorNum);
      }
    },
  
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief test _query function with options
  ////////////////////////////////////////////////////////////////////////////////
  
    testQueryOptionsObjectTtlError : function () {
      var result = db._query({
        query : "FOR i IN 1..2000 RETURN i",
        ttl : 0.00001,
        batchSize : 1000, // default
      });
      var docs = [ ];
      try {
        while (result.hasNext()) {
          docs.push(result.next());
        }
        result = true;
        fail();
      } catch (e) {
        assertEqual(ERRORS.ERROR_CURSOR_NOT_FOUND.code, e.errorNum);
      }
    },

  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(QuerySuite);
return jsunity.done();
