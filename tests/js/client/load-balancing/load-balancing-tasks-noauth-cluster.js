/* jshint globalstrict:true, strict:true, maxlen: 5000 */
/* global assertTrue, assertFalse, assertEqual */

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
/// @author Dan Larkin-York
/// @author Copyright 2018, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");

const db = require("internal").db;
const request = require("@arangodb/request");
const url = require('url');
const _ = require("lodash");
const getCoordinatorEndpoints = require('@arangodb/test-helper').getCoordinatorEndpoints;

const servers = getCoordinatorEndpoints();

function TasksSuite () {
  'use strict';
  const cns = ["animals", "fruits"];
  const keys = [
    ["ant", "bird", "cat", "dog"],
    ["apple", "banana", "coconut", "date"]
  ];
  let cs = [];
  let coordinators = [];
  const baseTasksUrl = `/_api/tasks`;

  function sendRequest(method, endpoint, body, usePrimary) {
    let res;
    const i = usePrimary ? 0 : 1;
    try {
      const envelope = {
        json: true,
        method,
        url: `${coordinators[i]}${endpoint}`
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
    setUp: function() {
      coordinators = getCoordinatorEndpoints();
      if (coordinators.length < 2) {
        throw new Error('Expecting at least two coordinators');
      }

      cs = [];
      for (let i = 0; i < cns.length; i++) {
        db._drop(cns[i]);
        cs.push(db._create(cns[i]));
        assertTrue(cs[i].name() === cns[i]);
        for (let key in keys[i]) {
          cs[i].save({ _key: key });
        }
      }

      require("internal").wait(2);
    },

    tearDown: function() {
      db._drop(cns[0]);
      db._drop(cns[1]);
      cs = [];
      coordinators = [];
    },

    testTaskForwarding: function() {
      let url = baseTasksUrl;
      const task = {
        command: `const time = Date.now();`,
        period: 2
      };
      let result = sendRequest('POST', url, task, true);

      assertFalse(result === undefined || result === {});
      assertEqual(result.status, 200);
      assertFalse(result.body.id === undefined);
      assertEqual(result.body.period, 2);

      const taskId = result.body.id;
      url = `${baseTasksUrl}/${taskId}`;
      result = sendRequest('GET', url, {}, false);

      assertFalse(result === undefined || result === {});
      assertEqual(result.status, 200);
      assertEqual(taskId, result.body.id);

      require('internal').wait(5.0, false);

      url = `${baseTasksUrl}/${taskId}`;
      result = sendRequest('DELETE', url, {}, {}, false);

      assertFalse(result === undefined || result === {});
      assertFalse(result.body.error);
      assertEqual(result.status, 200);
    },

  };
}

jsunity.run(TasksSuite);

return jsunity.done();
