/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, assertTrue, assertEqual, assertNotEqual, arango */

// //////////////////////////////////////////////////////////////////////////////
// / @brief ArangoTransaction sTests
// /
// /
// / DISCLAIMER
// /
// / Copyright 2018 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Jan Steemann
// //////////////////////////////////////////////////////////////////////////////

let jsunity = require('jsunity');
let internal = require('internal');
let arangodb = require('@arangodb');
let fs = require('fs');
let pu = require('@arangodb/process-utils');
let db = arangodb.db;

function dumpIntegrationSuite () {
  'use strict';
  // this file is used by multiple, hence the checked structure is only in a subdirectory:
  const dumpDir = fs.join(process.env['dumpdirectory'], 'UnitTestsDumpSrc');
  const cn = 'UnitTestsDumpKeygen';

  return {
    testDumpUncompressed: function () {
      try {
        let tree = fs.listTree(dumpDir);
        assertNotEqual(-1, tree.indexOf("ENCRYPTION"));
        let data = fs.readFileSync(fs.join(dumpDir, "ENCRYPTION"));
        assertEqual("none", data.toString());
        
        const prefix = "UnitTestsDumpKeygen_24f160fff8671be21db71c5f77fd72ce";

        let structure = prefix + ".structure.json";
        if (!fs.isFile(fs.join(dumpDir, structure))) {
          structure = cn + ".structure.json";
        }

        assertTrue(fs.isFile(fs.join(dumpDir, structure)), structure);
        assertNotEqual(-1, tree.indexOf(structure));
        data = JSON.parse(fs.readFileSync(fs.join(dumpDir, structure)).toString());
        assertEqual(cn, data.parameters.name);
        
        assertEqual(-1, tree.indexOf(prefix + ".data.json.gz"));
        assertNotEqual(-1, tree.indexOf(prefix + ".data.json"));
        data = fs.readFileSync(fs.join(dumpDir, prefix + ".data.json")).toString().trim().split('\n');
        assertEqual(1000, data.length);
        data.forEach(function(line) {
          line = JSON.parse(line);
          assertEqual(2300, line.type);
          assertTrue(line.data.hasOwnProperty('_key'));
          assertTrue(line.data.hasOwnProperty('_rev'));
        });
      } finally {
      }
    }
  };
}

jsunity.run(dumpIntegrationSuite);

return jsunity.done();
