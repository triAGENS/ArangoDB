/* jshint strict: false, sub: true */
/* global print */
'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2016 ArangoDB GmbH, Cologne, Germany
// / Copyright 2014 triagens GmbH, Cologne, Germany
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
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Max Neunhoeffer
// //////////////////////////////////////////////////////////////////////////////

const functionsDocumentation = {
  'recovery': 'run recovery tests'
};
const optionsDocumentation = [
];

const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const tu = require('@arangodb/testutils/test-utils');
const inst = require('@arangodb/testutils/instance');
const _ = require('lodash');

const toArgv = require('internal').toArgv;

const RED = require('internal').COLORS.COLOR_RED;
const RESET = require('internal').COLORS.COLOR_RESET;
const BLUE = require('internal').COLORS.COLOR_BLUE;

const testPaths = {
  'recovery': [tu.pathForTesting('server/recovery')]
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: recovery
// //////////////////////////////////////////////////////////////////////////////

function runArangodRecovery (params) {
  let useEncryption = false;

  if (global.ARANGODB_CLIENT_VERSION) {
    let version = global.ARANGODB_CLIENT_VERSION(true);
    if (version.hasOwnProperty('enterprise-version')) {
      useEncryption = true;
    }
  }

  let additionalParams= {
    'log.foreground-tty': 'true',
    'database.ignore-datafile-errors': 'false', // intentionally false!
  };

  if (useEncryption) {
    // randomly turn on or off hardware-acceleration for encryption for both
    // setup and the actual test. given enough tests, this will ensure that we run
    // a good mix of accelerated and non-accelerated encryption code. in addition,
    // we shuffle between the setup and the test phase, so if there is any
    // incompatibility between the two modes, this will likely find it
    additionalParams['rocksdb.encryption-hardware-acceleration'] = (Math.random() * 100 >= 50) ? "true" : "false";
  }

  let argv = [];

  let binary = pu.ARANGOD_BIN;
  params.crashLogDir = fs.join(fs.getTempPath(), 'crash');

  let crashLog = fs.join(params.crashLogDir, 'crash.log');

  if (params.setup) {
    additionalParams['javascript.script-parameter'] = 'setup';
    try {
      // clean up crash log before next test
      fs.remove(crashLog);
    } catch (err) {}


    // special handling for crash-handler recovery tests
    if (params.script.match(/crash-handler/)) {
      // forcefully enable crash handler, even if turned off globally
      // during testing
      require('internal').env["ARANGODB_OVERRIDE_CRASH_HANDLER"] = "on";
    }

    // enable development debugging if extremeVerbosity is set
    let args = {};
    if (params.options.extremeVerbosity === true) {
      args['log.level'] = 'development=info';
    }
    args = Object.assign(args, params.options.extraArgs);
    args = Object.assign(args, {
      'rocksdb.wal-file-timeout-initial': 10,
      'server.rest-server': 'false',
      'replication.auto-start': 'true',
      'javascript.script': params.script
    });
    
    args['log.output'] = 'file://' + crashLog;

    if (useEncryption) {
      params.keyDir = fs.join(fs.getTempPath(), 'arango_encryption');
      if (!fs.exists(params.keyDir)) {  // needed on win32
        fs.makeDirectory(params.keyDir);
      }
      
      const key = '01234567890123456789012345678901';
      
      let keyfile = fs.join(params.keyDir, 'rocksdb-encryption-keyfile');
      fs.write(keyfile, key);

      // special handling for encryption-keyfolder tests
      if (params.script.match(/encryption-keyfolder/)) {
        args['rocksdb.encryption-keyfolder'] = params.keyDir;
        process.env["rocksdb-encryption-keyfolder"] = params.keyDir;
      } else {
        args['rocksdb.encryption-keyfile'] = keyfile;
        process.env["rocksdb-encryption-keyfile"] = keyfile;
      }
    }
    params.options.disableMonitor = true;
    params.testDir = fs.join(params.tempDir, `${params.count}`);
    params['instance'] = new inst.instance(params.options,
                                           inst.instanceRole.single,
                                           args, {}, 'tcp', params.testDir, '',
                                           new inst.agencyConfig(params.options, null));

    argv = toArgv(Object.assign(params.instance.args, additionalParams));
  } else {
    additionalParams['javascript.script-parameter'] = 'recovery';
    argv = toArgv(Object.assign(params.instance.args, additionalParams));
    
    if (params.options.rr) {
      binary = 'rr';
      argv.unshift(pu.ARANGOD_BIN);
    }
  }
  
  process.env["state-file"] = params.stateFile;
  process.env["crash-log"] = crashLog;
  process.env["isAsan"] = params.options.isAsan;
  params.instanceInfo.pid = pu.executeAndWait(
    binary,
    argv,
    params.options,
    'recovery',
    params.instance.rootDir,
    !params.setup && params.options.coreCheck);
}

function recovery (options) {
  if (!global.ARANGODB_CLIENT_VERSION(true)['failure-tests'] ||
      global.ARANGODB_CLIENT_VERSION(true)['failure-tests'] === 'false') {
    return {
      recovery: {
        status: false,
        message: 'failure-tests not enabled. please recompile with -DUSE_FAILURE_TESTS=On'
      },
      status: false
    };
  }

  let results = {
    status: true
  };

  let recoveryTests = tu.scanTestPaths(testPaths.recovery, options);

  recoveryTests = tu.splitBuckets(options, recoveryTests);

  let count = 0;
  let orgTmp = process.env.TMPDIR;
  let tempDir = fs.join(fs.getTempPath(), 'recovery');
  fs.makeDirectoryRecursive(tempDir);
  process.env.TMPDIR = tempDir;

  for (let i = 0; i < recoveryTests.length; ++i) {
    let test = recoveryTests[i];
    let filtered = {};

    if (tu.filterTestcaseByOptions(test, options, filtered)) {
      count += 1;
      let iteration = 0;
      let stateFile = fs.getTempFile();

      while (true) {
        ++iteration;
        print(BLUE + "running setup #" + iteration + " of test " + count + " - " + test + RESET);
        let params = {
          tempDir: tempDir,
          instanceInfo: {
            rootDir: fs.join(fs.getTempPath(), 'recovery', count.toString())
          },
          options: _.cloneDeep(options),
          script: test,
          setup: true,
          count: count,
          testDir: "",
          stateFile,
          crashLogDir: "",
          keyDir: ""          
        };
        runArangodRecovery(params);

        ////////////////////////////////////////////////////////////////////////
        print(BLUE + "running recovery #" + iteration + " of test " + count + " - " + test + RESET);
        params.options.disableMonitor = options.disableMonitor;
        params.setup = false;
        try {
          tu.writeTestResult(params.instance.args['temp.path'], {
            failed: 1,
            status: false, 
            message: "unable to run recovery test " + test,
            duration: -1
          });
        } catch (er) {}
        runArangodRecovery(params);

        results[test] = tu.readTestResult(
          params.instance.args['temp.path'],
          {
            status: false
          },
          test
        );
        if (!results[test].status) {
          print("Not cleaning up " + params.testDir);
          results.status = false;
          // end while loop
          break;
        } 

        // check if the state file has been written by the test.
        // if so, we will run another round of this test!
        try {
          if (String(fs.readFileSync(stateFile)).length) {
            print('Going into next iteration of recovery test');
            if (params.options.cleanup) {
              if (params.crashLogDir !== "") {
                fs.removeDirectoryRecursive(params.crashLogDir, true);
              }
              if (params.keyDir !== "") {
                fs.removeDirectoryRecursive(params.keyDir, true);
              }
              continue;
            }
          }
        } catch (err) {
        }
        // last iteration. break out of while loop
        if (params.options.cleanup) {
          if (params.crashLogDir !== "") {
            fs.removeDirectoryRecursive(params.crashLogDir, true);
          }
          if (params.keyDir !== "") {
            fs.removeDirectoryRecursive(params.keyDir, true);
          }
        }
        break;
      }
    } else {
      if (options.extremeVerbosity) {
        print('Skipped ' + test + ' because of ' + filtered.filter);
      }
    }
  }
  if (options.cleanup && results.status) {
    fs.removeDirectoryRecursive(tempDir, true);
  }
  process.env.TMPDIR = orgTmp;
  if (count === 0) {
    print(RED + 'No testcase matched the filter.' + RESET);
    return {
      ALLTESTS: {
        status: true,
        skipped: true
      },
      status: true
    };
  }

  return results;
}

exports.setup = function (testFns, defaultFns, opts, fnDocs, optionsDoc, allTestPaths) {
  Object.assign(allTestPaths, testPaths);
  testFns['recovery'] = recovery;
  for (var attrname in functionsDocumentation) { fnDocs[attrname] = functionsDocumentation[attrname]; }
  for (var i = 0; i < optionsDocumentation.length; i++) { optionsDoc.push(optionsDocumentation[i]); }
};
