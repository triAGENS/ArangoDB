/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertNull */

// //////////////////////////////////////////////////////////////////////////////
// / @brief tests for dump/reload
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
// / @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  var c, i, j;
  for (i = 0; i < 10; ++i) {
    db._drop('UnitTestsRecovery' + i);
    // use fake collection ids
    c = db._create('UnitTestsRecovery' + i, { 'id': `${9999990 + i}` });
    for (j = 0; j < 10; ++j) {
      c.save({ _key: 'test' + j, value: j });
    }
  }

  // drop 'em all
  for (i = 0; i < 10; ++i) {
    db._drop('UnitTestsRecovery' + i);
  }
  internal.wait(5);

  for (i = 0; i < 10; ++i) {
    c = db._create('UnitTestsRecoveryX' + i, { 'id': `${9999990 + i}` });
    for (j = 0; j < 10; ++j) {
      c.save({ _key: 'test' + j, value: 'X' + j });
    }
  }

  // drop 'em all
  for (i = 0; i < 10; ++i) {
    db._drop('UnitTestsRecoveryX' + i);
  }
  internal.wait(5);

  for (i = 0; i < 10; ++i) {
    c = db._create('UnitTestsRecoveryY' + i, { 'id': `${9999990 + i}` });
    for (j = 0; j < 10; ++j) {
      c.save({ _key: 'test' + j, value: 'peterY' + j });
    }
  }

  db._drop('test');
  c = db._create('test');
  c.save({ _key: 'crashme' }, true);

  internal.debugTerminate('crashing server');
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {
    setUp: function () {},
    tearDown: function () {},

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test whether we can restore the trx data
    // //////////////////////////////////////////////////////////////////////////////

    testCollectionsReuse: function () {
      var c, i, j, doc;

      for (i = 0; i < 10; ++i) {
        assertNull(db._collection('UnitTestsRecovery' + i));
        assertNull(db._collection('UnitTestsRecoverX' + i));

        c = db._collection('UnitTestsRecoveryY' + i);
        assertEqual(10, c.count());

        for (j = 0; j < 10; ++j) {
          doc = c.document('test' + j);
          assertEqual('peterY' + j, doc.value);
        }
      }
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
