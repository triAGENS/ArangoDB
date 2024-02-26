/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertFalse */

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
/// @author Andrey Abramov
/// @author Copyright 2022, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();
  var i;

  db._drop('UnitTestsDummy');
  var c = db._create('UnitTestsDummy');
  var i1 = c.ensureIndex({ type: "inverted", name: "i1", fields: [ "num" ] });
  var meta1 = { indexes: [ { index: i1.name, collection: c.name() } ] };
  var i2 = c.ensureIndex({ type: "inverted", name: "i2", fields: [ "num1" ] });
  var meta2 = { indexes: [ { index: i2.name, collection: c.name() } ] };

  db._dropView('UnitTestsRecovery1');
  db._dropView('UnitTestsRecovery2');
  var v = db._createView('UnitTestsRecovery1', 'search-alias', meta1);

  v.rename('UnitTestsRecovery2');

  v = db._createView('UnitTestsRecovery1', 'search-alias', {});
  v.properties(meta2);

  db.UnitTestsDummy.save({ _key: 'foo' }, { waitForSync: true });

}

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    testViewRenameRecreate: function () {
      let checkView = function(viewName, indexName) {
        let v = db._view(viewName);
        assertEqual(v.name(), viewName);
        assertEqual(v.type(), 'search-alias');
        let indexes = v.properties().indexes;
        assertEqual(1, indexes.length);
        assertEqual(indexName, indexes[0].index);
        assertEqual("UnitTestsDummy", indexes[0].collection);
      };
      checkView("UnitTestsRecovery1", "i2");
      checkView("UnitTestsRecovery2", "i1");
    }
  };
}

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    runSetup();
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}
