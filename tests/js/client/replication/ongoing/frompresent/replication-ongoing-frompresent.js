/* jshint globalstrict:false, strict:false, unused: false */
/* global assertEqual, assertTrue, assertFalse, arango, ARGUMENTS */

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
const arangodb = require('@arangodb');
const db = arangodb.db;
const replication = require('@arangodb/replication');
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const compareTicks = replication.compareTicks;
const console = require('console');
const internal = require('internal');
let IM = global.instanceManager;
const leaderEndpoint = IM.arangods[0].endpoint;
const followerEndpoint = IM.arangods[1].endpoint;

const cn = 'UnitTestsReplication';
const cn2 = 'UnitTestsReplication2';

let connectToLeader = function () {
  reconnectRetry(leaderEndpoint, db._name(), 'root', '');
  db._flushCache();
};

let connectToFollower = function () {
  reconnectRetry(followerEndpoint, db._name(), 'root', '');
  db._flushCache();
};

let collectionChecksum = function (name) {
  var c = db._collection(name).checksum(true, true);
  return c.checksum;
};

let collectionCount = function (name) {
  return db._collection(name).count();
};

let compare = function (leaderFunc, leaderFunc2, followerFuncOngoing, followerFuncFinal, applierConfiguration) {
  let state = {};

  db._flushCache();
  leaderFunc(state);

  connectToFollower();
  replication.applier.stop();
  replication.applier.forget();

  while (replication.applier.state().state.running) {
    internal.wait(0.1, false);
  }

  var includeSystem = true;
  var restrictType = '';
  var restrictCollections = [];
  applierConfiguration = applierConfiguration || { };

  if (typeof applierConfiguration === 'object') {
    if (applierConfiguration.hasOwnProperty('includeSystem')) {
      includeSystem = applierConfiguration.includeSystem;
    }
    if (applierConfiguration.hasOwnProperty('restrictType')) {
      restrictType = applierConfiguration.restrictType;
    }
    if (applierConfiguration.hasOwnProperty('restrictCollections')) {
      restrictCollections = applierConfiguration.restrictCollections;
    }
  }

  var keepBarrier = false;
  if (applierConfiguration.hasOwnProperty('keepBarrier')) {
    keepBarrier = applierConfiguration.keepBarrier;
  }

  var syncResult = replication.sync({
    endpoint: leaderEndpoint,
    username: 'root',
    password: '',
    verbose: true,
    includeSystem,
    restrictType,
    restrictCollections,
    keepBarrier
  });

  assertTrue(syncResult.hasOwnProperty('lastLogTick'));

  connectToLeader();
  leaderFunc2(state);

  //  use lastLogTick as of now
  state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;

  applierConfiguration = applierConfiguration || {};
  applierConfiguration.endpoint = leaderEndpoint;
  applierConfiguration.username = 'root';
  applierConfiguration.password = '';

  if (!applierConfiguration.hasOwnProperty('chunkSize')) {
    applierConfiguration.chunkSize = 16384;
  }

  connectToFollower();

  replication.applier.properties(applierConfiguration);
  if (keepBarrier) {
    replication.applier.start(syncResult.lastLogTick, syncResult.barrierId);
  } else {
    replication.applier.start(syncResult.lastLogTick);
  }

  var printed = false;

  while (true) {
    var r = followerFuncOngoing(state);
    if (r === 'wait') {
      //  special return code that tells us to hang on
      internal.wait(0.5, false);
      continue;
    }
    if (!r) {
      break;
    }

    var followerState = replication.applier.state();

    if (followerState.state.lastError.errorNum > 0) {
      console.topic('replication=error', 'follower has errored:', JSON.stringify(followerState.state.lastError));
      break;
    }

    if (!followerState.state.running) {
      console.topic('replication=error', 'follower is not running');
      break;
    }

    if (compareTicks(followerState.state.lastAppliedContinuousTick, state.lastLogTick) >= 0 ||
        compareTicks(followerState.state.lastProcessedContinuousTick, state.lastLogTick) >= 0) {
      console.topic('replication=debug',
                    'follower has caught up. syncResult.lastLogTick:', state.lastLogTick,
                    'followerState.lastAppliedContinuousTick:', followerState.state.lastAppliedContinuousTick,
                    'followerState.lastProcessedContinuousTick:', followerState.state.lastProcessedContinuousTick);
      break;
    }

    if (!printed) {
      console.topic('replication=debug', 'waiting for follower to catch up');
      printed = true;
    }
    internal.wait(0.5, false);
  }

  db._flushCache();
  followerFuncFinal(state);
};

let stopApplier = function (dbName) {
  connectToFollower();
  try {
    db._useDatabase(dbName);

    replication.applier.stop();
    replication.applier.forget();
  } catch (err) {
  }
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief Basic test definition
// //////////////////////////////////////////////////////////////////////////////

function BaseTestConfig () {
  'use strict';

  return {

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test require from present
    // //////////////////////////////////////////////////////////////////////////////

    testRequireFromPresentFalse: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          //  flush the wal logs on the leader so the start tick is not available
          //  anymore when we start replicating
          for (var i = 0; i < 30; ++i) {
            db._collection(cn).save({
              value: i
            });
            internal.wal.flush();
          }
          db._collection(cn).save({
            value: i
          });
          internal.wal.flush(true, true);
          internal.wait(6, false);
        },

        function (state) {
          return true;
        },

        function (state) {
          //  rocksdb keeps wal longer
          let cc = db._collection(cn).count();
          assertEqual(cc, 31, 'rocksdb must keep wal, documents not there');
        }, {
          requireFromPresent: false,
          keepBarrier: false
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test require from present
    // //////////////////////////////////////////////////////////////////////////////

    testRequireFromPresentTrue: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          //  flush the wal logs on the leader so the start tick is not available
          //  anymore when we start replicating
          for (var i = 0; i < 30; ++i) {
            db._collection(cn).save({
              value: i
            });
            internal.wal.flush();
          }
          internal.wal.flush(true, true);
          internal.wait(6, false);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(30, state.count);
        },

        function (state) {
          //  wait for follower applier to have started and run
          internal.wait(5, false);

          //  follower should not have stopped
          assertTrue(replication.applier.state().state.running);
          return true;
        },

        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        {
          requireFromPresent: true,
          keepBarrier: true
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test require from present, no barrier
    // //////////////////////////////////////////////////////////////////////////////

    testRequireFromPresentTrueNoBarrier: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          //  flush the wal logs on the leader so the start tick is not available
          //  anymore when we start replicating
          for (var i = 0; i < 30; ++i) {
            db._collection(cn).save({
              value: i
            });
            internal.wal.flush();
          }
          internal.wal.flush(true, true);
          internal.wait(6, false);
        },

        function (state) {
          //  wait for follower applier to have started and detect the mess
          return replication.applier.state().state.running;
        },

        function (state) {
          //  rocksdb keeps wal longer
          assertTrue(replication.applier.state().state.running, 'Applier should be running');
          assertEqual(db._collection(cn).count(), 30, 'rocksdb must keep wal');
        },
        {
          requireFromPresent: true,
          keepBarrier: false
        }
      );
    }

  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief Test suite for _system database
// //////////////////////////////////////////////////////////////////////////////

function ReplicationSuite () {
  let suite = {

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief set up
    // //////////////////////////////////////////////////////////////////////////////

    setUp: function () {
      stopApplier('_system');

      connectToLeader();

      db._drop(cn);
      db._drop(cn2);
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief tear down
    // //////////////////////////////////////////////////////////////////////////////

    tearDown: function () {
      stopApplier('_system');

      connectToLeader();
      db._drop(cn);
      db._drop(cn2);

      connectToFollower();
      db._drop(cn);
      db._drop(cn2);
    }
  };
  deriveTestSuite(BaseTestConfig(), suite, '_Repl');

  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief Test suite for other database
// //////////////////////////////////////////////////////////////////////////////

function ReplicationOtherDBSuite () {
  const dbName = 'UnitTestDB';

  let suite = {

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief set up
    // //////////////////////////////////////////////////////////////////////////////

    setUp: function () {
      stopApplier(dbName);

      db._useDatabase('_system');
      connectToFollower();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }

      db._createDatabase(dbName);

      connectToLeader();

      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
      db._createDatabase(dbName);
      db._useDatabase(dbName);
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief tear down
    // //////////////////////////////////////////////////////////////////////////////

    tearDown: function () {
      stopApplier(dbName);

      db._useDatabase('_system');
      connectToLeader();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }

      db._useDatabase('_system');
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
    }
  };

  deriveTestSuite(BaseTestConfig(), suite, '_OtherRepl');
  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

jsunity.run(ReplicationSuite);
jsunity.run(ReplicationOtherDBSuite);

return jsunity.done();
