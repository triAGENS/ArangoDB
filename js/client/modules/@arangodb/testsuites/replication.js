/* jshint strict: false, sub: true */
/* global print, arango */
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
// / @author Max Neunhoeffer
// //////////////////////////////////////////////////////////////////////////////

const functionsDocumentation = {
  'replication_fuzz': 'replication randomized tests for all operations',
  'replication_random': 'replication randomized tests for transactions',
  'replication_aql': 'replication AQL tests',
  'replication_ongoing': 'replication ongoing tests',
  'replication_ongoing_global': 'replication ongoing "global" tests',
  'replication_ongoing_global_spec': 'replication ongoing "global-spec" tests',
  'replication_ongoing_frompresent': 'replication ongoing "frompresent" tests',
  'replication_static': 'replication static tests',
  'replication_sync': 'replication sync tests',
  'shell_replication': 'shell replication tests',
  'http_replication': 'client replication API tests'
};
const optionsDocumentation = [
];

const _ = require('lodash');
const pu = require('@arangodb/testutils/process-utils');
const tu = require('@arangodb/testutils/test-utils');
const trs = require('@arangodb/testutils/testrunners');
const ct = require('@arangodb/testutils/client-tools');

const testPaths = {
  'shell_replication': [tu.pathForTesting('common/replication')],
  'replication_aql': [tu.pathForTesting('client/replication/aql')],
  'replication_fuzz': [tu.pathForTesting('client/replication/fuzz')],
  'replication_random': [tu.pathForTesting('client/replication/random')],
  'replication_ongoing': [tu.pathForTesting('client/replication/ongoing')],
  'replication_ongoing_global': [tu.pathForTesting('client/replication/ongoing/global')],
  'replication_ongoing_global_spec': [tu.pathForTesting('client/replication/ongoing/global/spec')],
  'replication_ongoing_frompresent': [tu.pathForTesting('client/replication/ongoing/frompresent')],
  'replication_static': [tu.pathForTesting('client/replication/static')],
  'replication_sync': [tu.pathForTesting('client/replication/sync')],
  'http_replication': [tu.pathForTesting('common/replication_api')]
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: shell_replication
// //////////////////////////////////////////////////////////////////////////////

function shellReplication (options) {
  let testCases = tu.scanTestPaths(testPaths.shell_replication, options);

  var opts = {
    'replication': true
  };
  _.defaults(opts, options);

  return new trs.runOnArangodRunner(opts, 'shell_replication').run(testCases);
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: shell_replication
// //////////////////////////////////////////////////////////////////////////////

function shellClientReplicationApi (options) {
  let testCases = tu.scanTestPaths(testPaths.http_replication, options);

  var opts = {
    'replication': true,
  };
  
  arango.forceJson(true);
  _.defaults(opts, options);
  opts.forceJson = true;

  let ret = new trs.runLocalInArangoshRunner(opts, 'shell_replication_api').run(testCases);
  if (!options.forceJson) {
    arango.forceJson(false);
  }
  return ret;
}


class replicationRunner extends trs.runLocalInArangoshRunner {
  constructor(options, testname, serverOptions, startReplication=false) {
    super(options, testname, serverOptions);
    this.options.singles = 2;
    this.follower = undefined;
    this.addArgs = {};
    this.startReplication = startReplication;
  }

  preStart() {
    // our tests lean on accessing the `_users` collection, hence no auth for secondary
    this.instanceManager.arangods[1].args['server.authentication'] = false;
    return {
      message: '',
      state: true,
    };
  }
  postStart() {
    let message;
    print("starting replication follower: ");
    let state = true;
    this.addArgs['flatCommands'] = [this.instanceManager.arangods[1].endpoint];
    if (this.startReplication) {
      let res = ct.run.arangoshCmd(this.options, this.instanceManager,
                                   {}, [
        '--javascript.execute-string',
        `
          var users = require("@arangodb/users");
          users.save("replicator-user", "replicator-password", true);
          users.grantDatabase("replicator-user", "_system");
          users.grantCollection("replicator-user", "_system", "*", "rw");
          users.reload();
          `
      ],
                                   this.options.coreCheck);
      state = res.status;
    }
    return {
      message: message,
      state: state,
    };
  }
};


// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_fuzz
// //////////////////////////////////////////////////////////////////////////////

function replicationFuzz (options) {
  let testCases = tu.scanTestPaths(testPaths.replication_fuzz, options);
  return new replicationRunner(options, 'replication_fuzz', {"rocksdb.wal-file-timeout-initial": "7200"}).run(testCases);
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_random
// //////////////////////////////////////////////////////////////////////////////

function replicationRandom (options) {
  let testCases = tu.scanTestPaths(testPaths.replication_random, options);
  return new replicationRunner(options, 'replication_random').run(testCases);
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_aql
// //////////////////////////////////////////////////////////////////////////////

function replicationAql (options) {
  let testCases = tu.scanTestPaths(testPaths.replication_aql, options);
  return new replicationRunner(options, 'replication_aql').run(testCases);
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_ongoing*
// //////////////////////////////////////////////////////////////////////////////

var _replicationOngoing = function(path) {
  this.func = function replicationOngoing (options) {
    let testCases = tu.scanTestPaths(testPaths[path], options);
    return new replicationRunner(options, path).run(testCases);
  };
};

const replicationOngoing = (new _replicationOngoing('replication_ongoing')).func;
const replicationOngoingGlobal = (new _replicationOngoing('replication_ongoing_global')).func;
const replicationOngoingGlobalSpec = (new _replicationOngoing('replication_ongoing_global_spec')).func;
const replicationOngoingFrompresent = (new _replicationOngoing('replication_ongoing_frompresent')).func;

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_static
// //////////////////////////////////////////////////////////////////////////////

function replicationStatic (options) {
  let testCases = tu.scanTestPaths(testPaths.replication_static, options);

  return new replicationRunner(
    options,
    'leader_static',
    {
      'server.authentication': 'true'
    }, true).run(testCases);
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief TEST: replication_sync
// //////////////////////////////////////////////////////////////////////////////

function replicationSync (options) {
  let testCases = tu.scanTestPaths(testPaths.replication_sync, options);
  testCases = tu.splitBuckets(options, testCases);

  return new replicationRunner(options, 'replication_sync', {"server.authentication": "true"}).run(testCases);
}

exports.setup = function (testFns, opts, fnDocs, optionsDoc, allTestPaths) {
  Object.assign(allTestPaths, testPaths);
  testFns['shell_replication'] = shellReplication;
  testFns['replication_aql'] = replicationAql;
  testFns['replication_fuzz'] = replicationFuzz;
  testFns['replication_random'] = replicationRandom;
  testFns['replication_ongoing'] = replicationOngoing;
  testFns['replication_ongoing_global'] = replicationOngoingGlobal;
  testFns['replication_ongoing_global_spec'] = replicationOngoingGlobalSpec;
  testFns['replication_ongoing_frompresent'] = replicationOngoingFrompresent;
  testFns['replication_static'] = replicationStatic;
  testFns['replication_sync'] = replicationSync;
  testFns['http_replication'] = shellClientReplicationApi;
  tu.CopyIntoObject(fnDocs, functionsDocumentation);
  tu.CopyIntoList(optionsDoc, optionsDocumentation);
};
