/* jshint globalstrict:true, strict:true, maxlen: 5000 */
/* global assertTrue, assertFalse, assertEqual, assertNotUndefined, assertNotEqual, require*/

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2018, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");

const db = require("internal").db;
const request = require("@arangodb/request");
const url = require('url');
const _ = require("lodash");
const isEnterprise = require("internal").isEnterprise();
const getCoordinators = require('@arangodb/test-helper').getCoordinators;

const servers = getCoordinators();

function KeyGeneratorSuite () {
  'use strict';
  const cn = 'UnitTestsCollection';
  let coordinators = [];

  function sendRequest(method, endpoint, body, headers, usePrimary) {
    let res;
    const i = usePrimary ? 0 : 1;
    try {
      const envelope = {
        json: true,
        method,
        url: `${coordinators[i]}${endpoint}`,
        headers,
      };
      if (method !== 'GET') {
        envelope.body = body;
      }
      res = request(envelope);
    } catch(err) {
      console.error(`Exception processing ${method} ${endpoint}`, err.stack);
      return {};
    }

    if (typeof res.body === "string") {
      if (res.body === "") {
        res.body = {};
      } else {
        res.body = JSON.parse(res.body);
      }
    }
    return res;
  }

  return {
    setUpAll: function() {
      coordinators = getCoordinators();
      if (coordinators.length < 2) {
        throw new Error('Expecting at least two coordinators');
      }
    },

    testPadded: function() {
      // check that the generated keys are sequential when we send the requests
      // via multiple coordinators.
      db._create(cn, { numberOfShards: 1, keyOptions: { type: "padded" } });

      try {
        let lastKey = null;
        let url = "/_api/document/" + cn;
        // send documents to both coordinators
        for (let i = 0; i < 10000; ++i) {
          let result = sendRequest('POST', url, /*payload*/ {}, {}, i % 2 === 0);
          assertEqual(result.status, 202);
          let key = result.body._key;
          assertTrue(key > lastKey || lastKey === null, { key, lastKey });
          lastKey = key;
        }
      } finally {
        db._drop(cn);
      }
    },
    
    testPaddedOnOneShard: function() {
      if (!isEnterprise) {
        return;
      }

      db._createDatabase(cn, { sharding: "single" });
      try {
        db._useDatabase(cn);

        // check that the generated keys are sequential when we send the requests
        // via multiple coordinators.
        db._create(cn, { keyOptions: { type: "padded" } });
        assertNotEqual("", db[cn].properties().distributeShardsLike);

        let lastKey = null;
        let url = "/_db/" + cn + "/_api/document/" + cn;
        // send documents to both coordinators
        for (let i = 0; i < 10000; ++i) {
          let result = sendRequest('POST', url, /*payload*/ {}, {}, i % 2 === 0);
          assertEqual(result.status, 202);
          let key = result.body._key;
          assertTrue(key > lastKey || lastKey === null, { key, lastKey });
          lastKey = key;
        }
      } finally {
        db._useDatabase("_system");
        db._dropDatabase(cn);
      }
    },
    
  };
}

jsunity.run(KeyGeneratorSuite);

return jsunity.done();
