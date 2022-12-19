/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertTrue, assertFalse, assertNull, fail, AQL_EXECUTE */
////////////////////////////////////////////////////////////////////////////////
/// @brief recovery tests for views
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2022 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License")
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
/// @author Andrey Abramov
/// @author Copyright 2022, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var arangodb = require('@arangodb');
var db = arangodb.db;
var internal = require('internal');
var jsunity = require('jsunity');
var transactionFailure = require('@arangodb/test-helper-common').transactionFailure;

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  db._drop('UnitTestsRecoveryDummy');
  var c = db._create('UnitTestsRecoveryDummy');

  db._dropView('UnitTestsRecoveryView');
  db._createView('UnitTestsRecoveryView', 'search-alias', {});

  var i1 = c.ensureIndex({ type: "inverted", name: "i1", fields: [ "a", "b", "c" ] });
  var meta = { indexes: [ { index: i1.name, collection: c.name() } ] };
  db._view('UnitTestsRecoveryView').properties(meta);

  internal.wal.flush(true, true);
  internal.debugSetFailAt("RocksDBBackgroundThread::run");
  internal.wait(2); // make sure failure point takes effect

  return transactionFailure(
    {
      collections: {
        write: ['UnitTestsRecoveryDummy']
      },
      action: function() {
        var db = require('@arangodb').db;
        var c = db.UnitTestsRecoveryDummy;
        for (let i = 0; i < 10000; i++) {
          c.save({ a: "foo_" + i, b: "bar_" + i, c: i });
        }
        throw new Error('intentional abort');
      },
      waitForSync: true
    },
    internal.errors.ERROR_TRANSACTION_INTERNAL.code,
    'Error: intentional abort',
    true,
    false);
}

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {
    setUp: function () {},
    tearDown: function () {},

    testIResearchLinkPopulateTransactionAbortNoFlushThread: function () {
      let checkView = function(viewName, indexName) {
        let v = db._view(viewName);
        assertEqual(v.name(), viewName);
        assertEqual(v.type(), 'search-alias');
        let indexes = v.properties().indexes;
        assertEqual(1, indexes.length);
        assertEqual(indexName, indexes[0].index);
        assertEqual("UnitTestsRecoveryDummy", indexes[0].collection);
      };
      checkView("UnitTestsRecoveryView", "i1");

      var result = db._query("FOR doc IN UnitTestsRecoveryView SEARCH doc.c >= 0 COLLECT WITH COUNT INTO length RETURN length").toArray();
      assertEqual(0, result[0]);
    }
  };
}

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    return runSetup();
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}
