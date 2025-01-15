/* jshint globalstrict:false, strict:false, unused: false */
/* global ARGUMENTS */

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
// / @author Copyright 2013, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const {
  assertEqual, assertFalse, assertInstanceOf, assertNotEqual,
  assertNotNull, assertNull, assertTrue, fail
} = jsunity.jsUnity.assertions;
const arangodb = require('@arangodb');
const errors = arangodb.errors;
const db = arangodb.db;
const _ = require('lodash');
const userManager = require("@arangodb/users");

const replication = require('@arangodb/replication');
const compareTicks = require('@arangodb/replication-common').compareTicks;
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const console = require('console');
const internal = require('internal');
const arango = internal.arango;

const cn = 'UnitTestsReplication';
const cn2 = 'UnitTestsReplication2';
const systemCn = '_UnitTestsReplicationSys';

// these must match the values in the Makefile!
const replicatorUser = 'replicator-user';
const replicatorPassword = 'replicator-password';
let IM = global.instanceManager;
const leaderEndpoint = IM.arangods[0].endpoint;
const followerEndpoint = IM.arangods[1].endpoint;

const connectToLeader = function() {
  reconnectRetry(IM.arangods[0].endpoint, db._name(), replicatorUser, replicatorPassword);
};

const connectToFollower = function() {
  reconnectRetry(IM.arangods[1].endpoint, db._name(), 'root', '');
};

function rtaMakeCheckDataSuite() {
  return {
    setUp: function() {
      connectToFollower();
      replication.applier.stop();
      let opts={
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
        verbose: true,
        includeSystem: true,
        //restrictType: restrictType,
        //restrictCollections: restrictCollections,
        waitForSyncTimeout: 120,
        keepBarrier: true
      };
      print(opts)
      var syncResult = replication.setupReplicationGlobal(opts);
      print(syncResult)
      //print('eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeepu')
    },

    tearDown: function() {
      connectToFollower();
      replication.applier.stop();
    },
    testRTAMakeData: function() {
      const fs = require('fs');
      let res = {'total':0, 'duration':0.0, 'status':true, message: '', 'failed': 0};
      let moreargv = ['--skip',
                      '050_,051_,070_,071_,102_,570_,900_,950_']; // this replication doesn't spawn automatic per database
      
      const ct = require('@arangodb/testutils/client-tools');
      let count = 0;
      let messages = [
        "creating data on leader",
        "checking data on leader",
        "checking data on follower",
        "cleaning up"
      ];
      [
        0, // makedata
        1, // checkdata
        1, // checkdata follower
        2  // clear data
      ].forEach(testCount => {
        count += 1;
        let logFile = fs.join(fs.getTempPath(), `rta_out_${count}.log`);
        if (count === 3) {
          IM.endpoint = IM.arangods[1].endpoint;
        }
        else {
          IM.endpoint = IM.arangods[0].endpoint;
        }
        let rc = ct.run.rtaMakedata(IM.options, IM, testCount, messages[count-1], logFile, moreargv);
        if (!rc.status) {
          let rx = new RegExp(/\\n/g);
          res.message += 'replication-static.js:\n' + fs.read(logFile).replace(rx, '\n');
          res.status = false;
          res.failed += 1;
        } else {
          fs.remove(logFile);
        }
        if (count === 1) {
          var state = {};
          state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;
          IM.arangods[1].connect();
          while (true) {
            var followerState = replication.applier.state();
            print(followerState)
            if (followerState.state.lastError.errorNum > 0) {
              console.topic("replication=error", "follower has errored:", JSON.stringify(followerState.state.lastError));
              throw JSON.stringify(followerState.state.lastError);
            }

            if (!followerState.state.running) {
              throw new Error(`replication=error follower is not running: ${JSON.stringify(followerState)}`);
            }

            if (compareTicks(followerState.state.lastAppliedContinuousTick, state.lastLogTick) >= 0 ||
                compareTicks(followerState.state.lastProcessedContinuousTick, state.lastLogTick) >= 0) { // ||
              console.topic("replication=debug", "follower has caught up. state.lastLogTick:", state.lastLogTick, "followerState.lastAppliedContinuousTick:", followerState.state.lastAppliedContinuousTick, "followerState.lastProcessedContinuousTick:", followerState.state.lastProcessedContinuousTick);
              break;
            }
            
            if (!printed) {
              console.topic("replication=debug", "waiting for follower to catch up");
              printed = true;
            }
            internal.wait(0.5, false);
          }
          
        }
      });
      if (res.failed > 0) {
        throw new Error(`test failed :\n${JSON.stringify(res)}`);
      }
    }
  };
}

jsunity.run(rtaMakeCheckDataSuite);
return jsunity.done();
