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
let fs = require('fs');

function dumpIntegrationSuite () {
  'use strict';
  // this file is used by multiple, hence the checked structure is only in a subdirectory:
  const dumpDir = process.env['dump-directory'];
  const cn = 'UnitTestsDumpEdges';
  const dbName = 'UnitTestsDumpSrc';

  return {
    testDumpUncompressed: function () {
      let tree = fs.listTree(dumpDir);
      assertNotEqual(-1, tree.indexOf("ENCRYPTION"), dumpDir);
      let data = fs.readFileSync(fs.join(dumpDir, "ENCRYPTION"));
      assertEqual("none", data.toString());
      
      const prefix = "UnitTestsDumpEdges_8a31b923e9407ab76b6ca41131b8acf1";

      let structure = fs.join(dbName, prefix + ".structure.json");
      let fullNameIndex = tree.indexOf(structure);
      if (fullNameIndex == -1) {
        structure = cn + ".structure.json";
        fullNameIndex = tree.indexOf(structure);
      }
      assertNotEqual(-1, fullNameIndex, fs.join(dumpDir, '*', structure));
      let structureFile = fs.join(dumpDir, tree[fullNameIndex]);

      assertTrue(fs.isFile(structureFile),"structure file does not exist: " + structureFile);
      assertNotEqual(-1, tree.indexOf(structure));
      data = JSON.parse(fs.readFileSync(fs.join(dumpDir, structure)).toString());
      assertEqual(cn, data.parameters.name);
      
      let files = tree.filter((f) => f.startsWith(fs.join(dbName, prefix)) && f.match(/(\.\d+)?\.data\.json$/));
      assertNotEqual(0, files.length, files);
      files.forEach((file) => {
        data = fs.readFileSync(fs.join(dumpDir, file)).toString().trim().split('\n');
        assertEqual(10, data.length, fs.join(dumpDir, file));
        data.forEach(function(line) {
          line = JSON.parse(line);
          assertTrue(line.hasOwnProperty('_key'));
          assertTrue(line.hasOwnProperty('_rev'));
        });
      });
      
      files = tree.filter((f) => f.startsWith(prefix) && f.match(/(\.\d+)?\.data\.json\.gz$/));
      assertEqual(0, files.length, files);
    }
  };
}

jsunity.run(dumpIntegrationSuite);

return jsunity.done();
