/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, assertTrue, assertFalse, assertEqual, assertNotUndefined, arango */

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
// //////////////////////////////////////////////////////////////////////////////

const internal = require('internal');
const fs = require("fs");
const jsunity = require("jsunity");
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const base = require(fs.join(internal.pathForTesting('client'),
                             'shell', 'shell-iterators.inc'));

function StandaloneAqlIteratorWriteSuite() {
  'use strict';

  const ctx = {
    query: (...args) => internal.db._query(...args),
    collection: (name) => internal.db[name],
    abort: () => {},
  };
  const permute = function(run) {
    [ {}, {intermediateCommitCount: 111} ].forEach((opts) => {
      run(ctx, opts);
    });
  };
  
  let suite = {};
  deriveTestSuite(base.IteratorWriteSuite(permute, true), suite, '_StandaloneAqlRead');
  return suite;
}

jsunity.run(StandaloneAqlIteratorWriteSuite);

return jsunity.done();
