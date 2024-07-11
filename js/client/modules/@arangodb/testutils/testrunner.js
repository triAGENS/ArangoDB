/* jshint strict: false, sub: true */
/* global print db arango */
'use strict';

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
// / @author Wilfried Goesgens
// //////////////////////////////////////////////////////////////////////////////

const _ = require('lodash');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const tu = require('@arangodb/testutils/test-utils');
const im = require('@arangodb/testutils/instance-manager');
const time = require('internal').time;
const sleep = require('internal').sleep;

const GREEN = require('internal').COLORS.COLOR_GREEN;
const RED = require('internal').COLORS.COLOR_RED;
const RESET = require('internal').COLORS.COLOR_RESET;
const YELLOW = require('internal').COLORS.COLOR_YELLOW;

let didSplitBuckets = false;
function isBucketized(testBuckets) {
  if (testBuckets === undefined || testBuckets === null) {
    return false;
  }
  let n = testBuckets.split('/');
  let r = parseInt(n[0]);
  let s = parseInt(n[1]);
  if (r === 1 && s === 0) {
    // we only have a single bucket - this is equivalent to not using bucketizing at all
    return false;
  }
  return true;
}
exports.sutFilters = {
  checkUsers: ["users"],
  checkCollections: ["tasks", "collections", "views", "graphs"],
  checkDBs: ["databases"]
};
class testRunner {
  constructor(options, testname, serverOptions = {}, disableChecks=[]) {
    if (isBucketized(options.testBuckets) && !didSplitBuckets) {
      throw new Error("You parametrized to split buckets, but this testsuite doesn't support it!!!");
    }
    this.addArgs = undefined;
    this.options = options;
    this.friendlyName = testname;
    this.serverOptions = serverOptions;
    if (this.serverOptions === undefined) {
      this.serverOptions = {};
    }
    this.testList = [];
    this.customInstanceInfos = {};
    this.memProfCounter = 0;
    this.env = {};
    this.results = {};
    this.continueTesting = true;
    this.instanceManager = undefined;
    this.cleanupChecks = this.loadSutChecks(disableChecks);
  }
  loadSutChecks(disableCheckFilter) {
    let sutCheckers = _.filter(fs.list(fs.join(__dirname, 'sutcheckers')),
                              function (p) {
                                let extension = p.substr(-3);
                                let basename = p.slice(0, -3);
                                return (
                                  (extension === '.js') &&
                                  (disableCheckFilter.filter(f => f === basename).length === 0)
                                );
                              }).sort();
    let ret = [];
    sutCheckers.forEach(fn => {
      try {
        let checker = require('@arangodb/testutils/sutcheckers/'+fn).checker;
        ret.push(new checker(this));
      } catch (x) {
        print('failed to load module ' + fn);
        throw x;
      }
    });
    return ret;
  }
  isFailed(te) {
    const result = this.results[this.translateResult(te)];
    return !result.status;
  }
  setResult(te, serverDead, res) {
    let orgRes = JSON.stringify(this.results[this.translateResult(te)]);
    this.results[this.translateResult(te)] = res;
    if (serverDead) {
      this.serverDead = true;
    }
    this.results[this.translateResult(te)].message += " - Original test status: \n" + orgRes;
  }
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief Hooks that you can overload to be invoked in different phases:
  // //////////////////////////////////////////////////////////////////////////////
  preStart() { return {state: true}; } // before launching the SUT
  startFailed() { return {state: true}; } // if launching the SUT failed..
  postStart() { return {state: true}; } // after successfull launch of the SUT
  preRun() { return {state: true}; } // before each test
  postRun() { return {state: true}; } // after each test
  preStop() { return {state: true}; } // before shutting down the SUT
  postStop() { return {state: true}; } // after shutting down the SUT
  alive() { return true; } // after each testrun, check whether the SUT is alaive and well
  translateResult(testName) { return testName; } // if you want to manipulate test file names...
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief checks whether the SUT is alive and well:
  // //////////////////////////////////////////////////////////////////////////////
  healthCheck() {
    if (this.instanceManager.checkInstanceAlive() &&
        this.alive()) {
      return true;
    } else {
      this.continueTesting = false;
      return false;
    }
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief something is wrong. cook a summary for the spectator.
  // //////////////////////////////////////////////////////////////////////////////
  abortTestOnError(te) {
    if (!this.results.hasOwnProperty('SKIPPED')) {
      print('oops! Skipping remaining tests, server is unavailable for testing.');
      let originalMessage;
      if (this.results.hasOwnProperty(te) && this.results[this.translateResult(te)].hasOwnProperty('message')) {
        originalMessage = this.results[this.translateResult(te)].message;
      }
      this.results['SKIPPED'] = {
        status: false,
        message: ""
      };
      this.results[this.translateResult(te)] = {
        status: false,
        message: 'server unavailable for testing. ' + originalMessage
      };
    } else {
      if (this.results['SKIPPED'].message !== '') {
        this.results['SKIPPED'].message += ', ';
      }
      this.results['SKIPPED'].message += te;
    }

    this.instanceManager.exitStatus = 'server is gone.';

  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief checks for cluster wellbeing.
  // //////////////////////////////////////////////////////////////////////////////
  loadClusterTestStabilityInfo(){
    try {
      if (this.instanceManager.hasOwnProperty('clusterHealthMonitorFile')) {
        let status = true;
        let slow = [];
        let buf = fs.readBuffer(this.instanceManager.clusterHealthMonitorFile);
        let lineStart = 0;
        let maxBuffer = buf.length;
        for (let j = 0; j < maxBuffer; j++) {
          if (buf[j] === 10) { // \n
            const line = buf.utf8Slice(lineStart, j);
            lineStart = j + 1;
            let val = JSON.parse(line);
            if (val.state === false) {
              slow.push(val);
              status = false;
            }
          }
        }
        if (!status) {
          print('found ' + slow.length + ' slow lines!');
          this.results.status = false;
          this.results.message += 'found ' + slow.length + ' slow lines!';
        } else {
          print('everything fast!');
        }
      }
    }
    catch(x) {
      print(x);
    }
  }

  restartSniff(testCase) {
    if (this.options.sniffFilter) {
      this.instanceManager.stopTcpDump();
      if (testCase.search(this.options.sniffFilter) >= 0){
        let split = testCase.split(fs.pathSeparator);
        let fn = testCase;
        if (split.length > 0) {
          fn = split[split.length - 1];
        }
        this.instanceManager.launchTcpDump(fn + "_");
      }
    }
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief hook to replace with your way to invoke one test; existing overloads:
  //   - trs.runOnArangodRunner - spawn the test on the arangod / coordinator via .js
  //   - trs.runInArangoshRunner - spawn an arangosh to launch the test in
  //   - trs.runLocalInArangoshRunner - eval the test into the current arangosh
  // //////////////////////////////////////////////////////////////////////////////
  runOneTest(testCase) {
    throw new Error("must overload the runOneTest function!");
  }

  filter(te, filtered) {
    return tu.filterTestcaseByOptions(te, this.options, filtered);
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Main loop - launch the SUT, iterate over testList, shut down
  run(testList) {
    this.continueTesting = true;
    this.testList = testList;
    if (this.testList.length === 0) {
      print('Testsuite is empty!');
      return {
        "ALLTESTS" : {
          status: ((this.options.skipGrey ||
                    this.options.skipTimecritial ||
                    this.options.skipNondeterministic) &&
                   (this.options.test === undefined)),
          skipped: true,
          message: 'no testfiles were found!'
        }
      };
    }
    
    let beforeStart = time();

    this.instanceManager = new im.instanceManager(this.options.protocol,
                                                  this.options,
                                                  this.serverOptions,
                                                  this.friendlyName);
    this.instanceManager.prepareInstance();
    this.customInstanceInfos['preStart'] = this.preStart();
    if (this.customInstanceInfos.preStart.state === false) {
      return {
        setup: {
          status: false,
          message: 'custom preStart failed!' + this.customInstanceInfos.preStart.message
        },
        shutdown: this.customInstanceInfos.preStart.shutdown
      };
    }

    this.instanceManager.launchTcpDump("");
    if (this.instanceManager.launchInstance() === false) {
      this.customInstanceInfos['startFailed'] = this.startFailed();
      return {
        setup: {
          status: false,
          message: 'failed to start server!'
        }
      };
    }
    this.instanceManager.reconnect();
    this.customInstanceInfos['postStart'] = this.postStart();
    if (this.customInstanceInfos.postStart.state === false) {
      let shutdownStatus = this.customInstanceInfos.postStart.shutdown;
      shutdownStatus = shutdownStatus && this.instanceManager.shutdownInstance();
      return {
        setup: {
          status: false,
          message: 'custom postStart failed: ' + this.customInstanceInfos.postStart.message,
          shutdown: shutdownStatus
        }
      };
    }

    let testrunStart = time();
    this.results = {
      failed: 0,
      shutdown: true,
      startupTime: testrunStart - beforeStart
    };
    let serverDead = false;
    let count = 0;
    let forceTerminate = false;
    let moreReason = "";
    for (let i = 0; i < this.testList.length; i++) {
      let te = this.testList[i];
      let filtered = {};

      if (this.filter(te, filtered)) {
        let first = true;
        let loopCount = 0;
        count += 1;
        
        for (let j = 0; j < this.cleanupChecks.length; j++) {
          if (!this.continueTesting || !this.cleanupChecks[j].setUp(te)) {
            this.continueTesting = false;
            print(RED + Date() + ' server pretest "' + this.cleanupChecks[j].name + '" failed!' + RESET);
            moreReason += `server pretest '${this.cleanupChecks[j].name}' failed!`;
            j = this.cleanupChecks.length;
            continue;
          }
        }
        while (first || this.options.loopEternal) {
          if (!this.continueTesting) {
            this.abortTestOnError(te);
            i = this.testList.length;
            break;
          }
          this.restartSniff(te);
          this.preRun(te);
          this.instanceManager.getMemProfSnapshot(this.memProfCounter++);
          
          print('\n' + (new Date()).toISOString() + GREEN + " [============] " + this.info + ': Trying', te, '... ' + count, RESET);
          let reply = this.runOneTest(te);
          if (reply.hasOwnProperty('forceTerminate') && reply.forceTerminate) {
            moreReason += "test told us that we should forceTerminate.";
            this.results[this.translateResult(te)] = reply;
            this.continueTesting = false;
            forceTerminate = true;
            continue;
          }

          if (reply.hasOwnProperty('status')) {
            this.results[this.translateResult(te)] = reply;

            if (this.results[this.translateResult(te)].status === false) {
              this.results.failed ++;
              this.options.cleanup = false;
            }

            if (!reply.status && !this.options.force) {
              break;
            }
          } else {
            this.results[this.translateResult(te)] = {
              status: false,
              message: reply
            };

            if (!this.options.force) {
              break;
            }
          }

          if (this.healthCheck()) {
            if (!this.options.disableClusterMonitor) {
              this.results[this.translateResult(te)]['processStats'] = this.instanceManager.getDeltaProcessStats();
            } else {
              this.results[this.translateResult(te)]['processStats'] = {};
            }
            this.results[this.translateResult(te)]['processStats']['netstat'] = this.instanceManager.getNetstat();
            this.continueTesting = true;
            let j = 0;
            try {
              for (; j < this.cleanupChecks.length; j++) {
                if (!this.continueTesting || !this.cleanupChecks[j].runCheck(te)) {
                  print(RED + Date() + ' server posttest "' + this.cleanupChecks[j].name + '" failed!' + RESET);
                  moreReason += `server posttest '${this.cleanupChecks[j].name}' failed!`;
                  this.continueTesting = false;
                  j = this.cleanupChecks.length;
                  continue;
                }
              }
            } catch(ex) {
              this.continueTesting = false;
              print(`${RED}${Date()} server posttest "${this.cleanupChecks[j].name}" failed by throwing: ${ex}\n${ex.stack}!${RESET}`);
              moreReason += `server posttest "${this.cleanupChecks[j].name}" failed by throwing: ${ex}`;
              continue;
            }
          } else {
            this.results[this.translateResult(te)].message = "Instance not healthy! " + JSON.stringify(reply);
            continue;
          }
          first = false;

          if (this.options.loopEternal) {
            if (loopCount % this.options.loopSleepWhen === 0) {
              print('sleeping...');
              sleep(this.options.loopSleepSec);
              print('continuing.');
            }

            ++loopCount;
          }
          this.postRun();
        }
      } else {
        if (this.options.extremeVerbosity) {
          print('Skipped ' + te + ' because of ' + filtered.filter);
        }
        this.results[this.translateResult(te)] = {
          status: true,
          skipped: true,
          message: filtered.filter
        };
      }
    }

    if (count === 0) {
      this.results['ALLTESTS'] = {
        status: true,
        skipped: true
      };
      this.results.status = true;
      print(RED + 'No testcase matched the filter.' + RESET);
    }

    if (this.options.sleepBeforeShutdown !== 0) {
      print("Sleeping for " + this.options.sleepBeforeShutdown + " seconds");
      sleep(this.options.sleepBeforeShutdown);
    }

    if (this.continueTesting) {
      this.instanceManager.getMemProfSnapshot(this.memProfCounter++);
    }

    let shutDownStart = time();
    this.results['testDuration'] = shutDownStart - testrunStart;
    if (!this.options.noStartStopLogs) {
      print(Date() + ' Shutting down...');
    }
    this.customInstanceInfos.preStop = this.preStop();
    if (this.customInstanceInfos.preStop.state === false) {
      if (!this.results.hasOwnProperty('setup')) {
        this.results['setup'] = {};
      }
      this.results.setup['status'] = false;
      this.results.setup['message'] = 'custom preStop failed!' + this.customInstanceInfos.preStop.message;
      if (this.customInstanceInfos.preStop.hasOwnProperty('shutdown')) {
        this.results.shutdown = this.results.shutdown && this.customInstanceInfos.preStop.shutdown;
      }
    }
    // pass on JWT secret
    let clonedOpts = _.clone(this.options);
    if (this.serverOptions['server.jwt-secret'] && !clonedOpts['server.jwt-secret']) {
      clonedOpts['server.jwt-secret'] = this.serverOptions['server.jwt-secret'];
    }
    this.results.shutdown = this.results.shutdown && this.instanceManager.shutdownInstance(forceTerminate, moreReason);
    if (!this.results.shutdown) {
      this.results.status = false;
    }

    this.loadClusterTestStabilityInfo();

    this.customInstanceInfos['postStop'] = this.postStop();
    if (this.customInstanceInfos.postStop.state === false) {
      this.results.setup.status = false;
      this.results.setup.message = 'custom postStop failed!';
    }
    this.results['shutdownTime'] = time() - shutDownStart;

    if (!this.options.noStartStopLogs) {
      print('done.');
    }
    this.instanceManager.destructor(this.continueTesting && this.results.failed === 0);
    return this.results;
  }
}

exports.testRunner = testRunner;
exports.setDidSplitBuckets = function (val) { didSplitBuckets = val; };
exports.registerOptions = function(optionsDefaults, optionsDocumentation) {
  tu.CopyIntoObject(optionsDefaults, {
    'loopEternal': false,
    'loopSleepSec': 1,
    'loopSleepWhen': 1,
    'sleepBeforeShutdown' : 0,
  });
  tu.CopyIntoList(optionsDocumentation, [
    ' Test loop control:',
    '   - `loopEternal`: to loop one test over and over.',
    '   - `loopSleepWhen`: sleep every nth iteration',
    '   - `loopSleepSec`: sleep seconds between iterations',
    '   - `sleepBeforeShutdown`: let the system rest before terminating it',
    ''
  ]);
};
