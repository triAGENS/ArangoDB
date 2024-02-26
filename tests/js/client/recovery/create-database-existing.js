/* jshint globalstrict:false, strict:false, unused : false */
/* global assertNotEqual, fail */

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
// / @author Jan Steemann
// / @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');
var fs = require('fs');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  try {
    db._dropDatabase('UnitTestsRecovery');
  } catch (err) {}

  db._createDatabase('UnitTestsRecovery');
  db._useDatabase('UnitTestsRecovery');
  var id = Number(db._id());
  var path = db._path().replace(/-\d+$/, '');
  db._useDatabase('_system');

  // create some empty directories
  for (var i = 1; i < 1000; ++i) {
    try {
      fs.makeDirectory(path + '-' + (id + i));
    } catch (err) {}
  }

}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test whether failures when creating databases causes issues
    // //////////////////////////////////////////////////////////////////////////////

    testCreateDatabaseExisting: function () {
      assertNotEqual(-1, db._databases().indexOf('UnitTestsRecovery'));
    }

  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

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
