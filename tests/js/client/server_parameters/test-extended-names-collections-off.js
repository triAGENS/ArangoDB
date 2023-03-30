/*jshint globalstrict:false, strict:false */
/* global getOptions, assertEqual, assertTrue, assertNull, assertNotNull, fail */

////////////////////////////////////////////////////////////////////////////////
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
/// Copyright holder is ArangoDB Inc, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2023, ArangoDB Inc, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

if (getOptions === true) {
  return {
    'database.extended-names-collections': "false",
  };
}
const jsunity = require('jsunity');
const db = require('internal').db;
const errors = require('@arangodb').errors;
const isCluster = require("internal").isCluster;

const traditionalName = "UnitTestsCollection";
const extendedName = "Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨";

function testSuite() {
  return {
    tearDown: function() {
      db._drop(traditionalName);
      db._drop(extendedName);
    },

    testCreateExtendedName: function() {
      try {
        db._create(extendedName);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_ILLEGAL_NAME.code, err.errorNum);
      }
      
      let c = db._collection(extendedName);
      assertNull(c);
    },
    
    testCreateEdgeExtendedName: function() {
      try {
        db._createEdgeCollection(extendedName);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_ILLEGAL_NAME.code, err.errorNum);
      }
      
      let c = db._collection(extendedName);
      assertNull(c);
    },
    
    testRenameToExtendedName: function() {
      if (isCluster()) {
        // renaming not supported in cluster
        return;
      }
        
      let c = db._create(traditionalName);
      try {
        c.rename(extendedName);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_ILLEGAL_NAME.code, err.errorNum);
      }
    },
    
  };
}

jsunity.run(testSuite);
return jsunity.done();
