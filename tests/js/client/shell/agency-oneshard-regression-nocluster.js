/*jshint globalstrict:false, strict:false */
/* global getOptions, assertEqual, assertFalse, assertTrue, arango, print */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const pu = require('@arangodb/testutils/process-utils');
const fs = require("fs");
const { executeExternalAndWaitWithSanitizer } = require('@arangodb/test-helper');

function UpgradeForceOneShardRegressionSuite() {
  'use strict';

  return {
    testCanStartAgencyWithAutoUpgradeAndForceOneShard: function () {

      /*
      * This tests a regression on Agent servers, that could not be restarted
      * with force-one-shard and auto-upgrade.
      * As we need to get controll over return code we start the agent as
      * an external process and wait for it to complete the upgrade routine
      * If it exits with anything other than ExitCode 0 it should print log
      * to std out for investigation.
      * The options we use here are the bare minimum to identify a server as
      * agent.
      */
      const arangod = pu.ARANGOD_BIN;
      assertTrue(fs.isFile(arangod), "arangod not found!");

      let addConnectionArgs = function (args) {
        const path = fs.getTempFile();
        args.push('--server.endpoint');
        args.push("none");
        args.push('--agency.activate');
        args.push('true');
        args.push('--agency.size');
        args.push('1');
        args.push('--agency.supervision');
        args.push('true');
        args.push('--cluster.force-one-shard');
        args.push('true');
        args.push('--database.auto-upgrade');
        args.push('true');
        args.push(path);
      };
      const args = [];
      addConnectionArgs(args);
      require("console").warn(`Start arangod agency with args: ${JSON.stringify(args)}`);
      const actualRc = executeExternalAndWaitWithSanitizer(arangod, args, 'agency_oneshard_regression-noncluster');
      assertEqual(actualRc.exit, 0, `Instead process exited with ${JSON.stringify(actualRc)}`);
    }
  };
}

//////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
//////////////////////////////////////////////////////////////////////////////

jsunity.run(UpgradeForceOneShardRegressionSuite);

return jsunity.done();
