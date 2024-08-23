/*jshint globalstrict:false, strict:false */
/* global fail, getOptions, assertTrue, assertEqual, assertNotEqual */

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
/// @author Wilfried Goesgens
/// @author Copyright 2019, ArangoDB Inc, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

if (getOptions === true) {
  return {
    'javascript.harden': true
  };
}

var jsunity = require('jsunity');

function testSuite() {
  const arangodb = require("@arangodb");
  const internal = require('internal');
  // want the statistics of this process, not the server remoting:
  const processStatistics = internal.thisProcessStatistics;
  const getPid = internal.getPid;
  const logLevel = internal.logLevel;

  return {
    testHardenedFunctionProcessStatistics : function() {
      try {
        processStatistics();
        fail();
      } catch (err) {
        //disabled for oasis
        //assertEqual(arangodb.ERROR_FORBIDDEN, err.errorNum);
      }
    },

    testHardenedFunctionGetPid : function() {
      try {
        getPid();
        fail();
      } catch (err) {
        assertEqual(arangodb.ERROR_FORBIDDEN, err.errorNum);
      }
    },

    testHardenedFunctionLogLevel : function() {
      try {
        logLevel();
        fail();
      } catch (err) {
        assertEqual(arangodb.ERROR_FORBIDDEN, err.errorNum);
      }
    }
  };
}
jsunity.run(testSuite);
return jsunity.done();
