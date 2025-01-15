/* jshint globalstrict:false, strict:false, unused: false */
/* global fail, assertEqual, assertTrue, assertFalse, assertNull, assertNotNull, arango, ARGUMENTS */

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
const analyzers = require("@arangodb/analyzers");
const db = arangodb.db;

const replication = require('@arangodb/replication');
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const compareTicks = replication.compareTicks;
const console = require('console');
const internal = require('internal');

const cn = 'UnitTestsReplication';
const cn2 = 'UnitTestsReplication2';

let IM = global.instanceManager;
const leaderEndpoint = IM.arangods[0].endpoint;
const followerEndpoint = IM.arangods[1].endpoint;

const connectToLeader = function() {
  reconnectRetry(IM.arangods[0].endpoint, db._name(), 'root', '');
  db._flushCache();
};

const connectToFollower = function() {
  reconnectRetry(IM.arangods[1].endpoint, db._name(), 'root', '');
  db._flushCache();
};

const collectionChecksum = function (name) {
  return db._collection(name).checksum(true, true).checksum;
};

const collectionCount = function (name) {
  return db._collection(name).count();
};

const compare = function (leaderFunc, leaderFunc2, followerFuncOngoing, followerFuncFinal, applierConfiguration) {
  let state = {};

  db._flushCache();
  leaderFunc(state);

  connectToFollower();
  replication.applier.stop();
  replication.applier.forget();

  while (replication.applier.state().state.running) {
    internal.wait(0.1, false);
  }

  let syncResult = replication.sync({
    endpoint: leaderEndpoint,
    username: 'root',
    password: '',
    verbose: true,
    includeSystem: true,
    keepBarrier: true
  });

  assertTrue(syncResult.hasOwnProperty('lastLogTick'));

  connectToLeader();
  leaderFunc2(state);

  // use lastLogTick as of now
  let loggerState = replication.logger.state().state;
  state.lastLogTick = loggerState.lastUncommittedLogTick;

  applierConfiguration = applierConfiguration || {};
  applierConfiguration.endpoint = leaderEndpoint;
  applierConfiguration.username = 'root';
  applierConfiguration.password = '';
  applierConfiguration.requireFromPresent = true;

  if (!applierConfiguration.hasOwnProperty('chunkSize')) {
    applierConfiguration.chunkSize = 16384;
  }

  connectToFollower();

  replication.applier.properties(applierConfiguration);
  replication.applier.start(syncResult.lastLogTick, syncResult.barrierId);

  let printed = false;
  let handled = false;

  let followerState;

  while (true) {
    if (!handled) {
      let r = followerFuncOngoing(state);
      if (r === 'wait') {
        // special return code that tells us to hang on
        internal.wait(0.5, false);
        continue;
      }

      handled = true;
    }

    followerState = replication.applier.state();

    if (followerState.state.lastError.errorNum > 0) {
      console.topic('replication=error', 'follower has errored:', JSON.stringify(followerState.state.lastError));
      throw JSON.stringify(followerState.state.lastError);
    }

    if (!followerState.state.running) {
      console.topic('replication=error', 'follower is not running');
      break;
    }

    if (compareTicks(followerState.state.lastAppliedContinuousTick, state.lastLogTick) >= 0 ||
        compareTicks(followerState.state.lastProcessedContinuousTick, state.lastLogTick) >= 0) { // ||
        console.topic('replication=debug',
                      'follower has caught up. state.lastLogTick:', state.lastLogTick,
                      'followerState.lastAppliedContinuousTick:', followerState.state.lastAppliedContinuousTick,
                      'followerState.lastProcessedContinuousTick:', followerState.state.lastProcessedContinuousTick);
      break;
    }

    if (!printed) {
      console.topic('replication=debug', 'waiting for follower to catch up');
      printed = true;
    }
    internal.wait(0.25, false);
  }

  db._flushCache();

  try {
    followerFuncFinal(state);
  } catch (err) {
    console.warn("caught error. debug information: syncResult:", syncResult, "loggerState:", loggerState, "followerState:", followerState.state);
    throw err;
  }
};

const checkCountConsistency = function(cn, expected) {
  let check = function() {
    db._flushCache();
    let c = db[cn];
    let figures = c.figures(true).engine;

    assertEqual(expected, c.count());
    assertEqual(expected, c.toArray().length);
    assertEqual(expected, figures.documents);
    assertEqual("primary", figures.indexes[0].type);
    assertEqual(expected, figures.indexes[0].count);
    figures.indexes.forEach((idx) => {
      assertEqual(expected, idx.count);
    });
  };
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief Base Test Config. Identitical part for _system and other DB
// //////////////////////////////////////////////////////////////////////////////

function BaseTestConfig () {
  'use strict';

  return {
    testStatsInsert: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({ _key: "test" + i });
          }
          c.insert(docs);
        },

        function (state) {
        },

        function (state) {
          let s = replication.applier.state().state;
          assertTrue(s.totalDocuments >= 1000);
          assertTrue(s.totalFetchTime > 0);
          assertTrue(s.totalApplyTime > 0);
          assertTrue(s.averageApplyTime > 0);
        }
      );
    },
    
    testStatsInsertUpdate: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({ _key: "test" + i });
          }
          c.insert(docs);
        },

        function (state) {
          for (let i = 0; i < 1000; ++i) {
            db._collection(cn).update("test" + i, { value: i });
          }
        },

        function (state) {
        },

        function (state) {
          let s = replication.applier.state().state;
          assertTrue(s.totalDocuments >= 1000);
          assertTrue(s.totalFetchTime > 0);
          assertTrue(s.totalApplyTime > 0);
          assertTrue(s.averageApplyTime > 0);
  
          checkCountConsistency(cn, 1000);
        }
      );
    },
    
    testStatsRemove: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({ _key: "test" + i });
          }
          c.insert(docs);
          for (let i = 0; i < 1000; ++i) {
            c.remove("test" + i);
          }
        },

        function (state) {
        },

        function (state) {
          let s = replication.applier.state().state;
          assertTrue(s.totalDocuments >= 1000);
          assertTrue(s.totalRemovals >= 1000);
          assertTrue(s.totalFetchTime > 0);
          assertTrue(s.totalApplyTime > 0);
          assertTrue(s.averageApplyTime > 0);
        }
      );
    },

    testIncludeCollection: function () {
      connectToLeader();

      compare(
        function (state) {
          db._drop(cn);
          db._drop(cn + '2');
        },

        function (state) {
          db._create(cn);
          db._create(cn + '2');
          for (let i = 0; i < 100; ++i) {
            db._collection(cn).insert({ value: i });
            db._collection(cn + '2').insert({ value: i });
          }
        },

        function (state) {
        },

        function (state) {
          assertEqual(100, db._collection(cn).count());
          assertNull(db._collection(cn + '2'));
        },

        {
          restrictType: 'include',
          restrictCollections: [cn]
        }
      );
    },

    testExcludeCollection: function () {
      connectToLeader();

      compare(
        function (state) {
          db._drop(cn);
          db._drop(cn + '2');
        },

        function (state) {
          db._create(cn);
          db._create(cn + '2');
          for (let i = 0; i < 100; ++i) {
            db._collection(cn).insert({ value: i });
            db._collection(cn + '2').insert({ value: i });
          }
        },

        function (state) {
        },

        function (state) {
          assertEqual(100, db._collection(cn).count());
          assertNull(db._collection(cn + '2'));
        },

        {
          restrictType: 'exclude',
          restrictCollections: [cn + '2']
        }
      );
    },
    
    testInsertRemoveInsert: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },
        function (state) {
          for (let i = 0; i < 1000; ++i) {
            db._collection(cn).insert({ _key: "test" + i, value: i });
            db._collection(cn).update("test" + i, { value: i + 1 });
            db._collection(cn).remove("test" + i);
            db._collection(cn).insert({ _key: "test" + i, value: 42 + i });
          }
        },

        function (state) {
        },

        function (state) {
          checkCountConsistency(cn, 1000);
        }
      );
    },
    
    testInsertRemoveTransaction: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },
        function (state) {
          const opts = {
            collections: {
              write: [cn]
            }
          };
          const trx = internal.db._createTransaction(opts);

          const tc = trx.collection(cn);
          for (let i = 0; i < 1000; ++i) {
            tc.insert({ _key: "test" + i, value: i });
            tc.update("test" + i, { value: i + 1 });
            tc.remove("test" + i);
            tc.insert({ _key: "test" + i, value: 42 + i });
          }
          trx.commit();
        },

        function (state) {
        },

        function (state) {
          checkCountConsistency(cn, 1000);
        }
      );
    },
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test duplicate _key issue and replacement
    // //////////////////////////////////////////////////////////////////////////////

    testPrimaryKeyConflict: function () {
      connectToLeader();

      compare(
        function (state) {
          db._drop(cn);
          db._create(cn);
        },

        function (state) {
          // insert same record on follower that we will insert on the leader
          connectToFollower();
          db[cn].insert({
            _key: 'boom',
            who: 'follower'
          }, {waitForSync: true});
          console.warn("leader state:", replication.logger.state().state);  
          assertEqual('follower', db[cn].document('boom').who);

          connectToLeader();
          db[cn].insert({
            _key: 'boom',
            who: 'leader'
          }, {waitForSync: true});
          assertEqual('leader', db[cn].document('boom').who);
        },

        function (state) {
        },

        function (state) {
          // leader document version must have one
          assertEqual('leader', db[cn].document('boom').who);
          checkCountConsistency(cn, 1);
        }
      );
    },

    testPrimaryKeyConflictInTransaction: function() {
      connectToLeader();

      compare(
        function(state) {
          db._drop(cn);
          db._create(cn);
        },

        function(state) {
          // insert same record on follower that we will insert on the leader
          connectToFollower();
          db[cn].insert({ _key: "boom", who: "follower" });
          connectToLeader();
          db._executeTransaction({ 
            collections: { write: cn },
            action: function(params) {
              let db = require("internal").db;
              db[params.cn].insert({ _key: "meow", foo: "bar" });
              db[params.cn].insert({ _key: "boom", who: "leader" }, {waitForSync: true});
            },
            params: { cn }
          });
        },

        function(state) {
        },

        function(state) {
          assertEqual(2, db[cn].count());
          assertEqual("bar", db[cn].document("meow").foo);
          // leader document version must have won
          assertEqual("leader", db[cn].document("boom").who);
          checkCountConsistency(cn, 2);
        }
      );
    },

    testSecondaryKeyConflict: function () {
      connectToLeader();

      compare(
        function (state) {
          db._drop(cn);
          db._create(cn);
          db[cn].ensureIndex({
            type: 'persistent',
            fields: ['value'],
            unique: true
          });
        },

        function (state) {
          // insert same record on follower that we will insert on the leader
          connectToFollower();
          db[cn].insert({
            _key: 'follower',
            value: 'one'
          });
          connectToLeader();
          db[cn].insert({
            _key: 'leader',
            value: 'one'
          });
        },

        function (state) {
        },

        function (state) {
          assertNull(db[cn].firstExample({
            _key: 'follower'
          }));
          assertNotNull(db[cn].firstExample({
            _key: 'leader'
          }));
          assertEqual('leader', db[cn].toArray()[0]._key);
          assertEqual('one', db[cn].toArray()[0].value);
          checkCountConsistency(cn, 1);
        }
      );
    },

    testSecondaryKeyConflictInTransaction: function() {
      connectToLeader();

      compare(
        function(state) {
          db._drop(cn);
          db._create(cn);
          db[cn].ensureIndex({ type: "persistent", fields: ["value"], unique: true });
        },

        function(state) {
          // insert same record on follower that we will insert on the leader
          connectToFollower();
          db[cn].insert({ _key: "follower", value: "one" });
          connectToLeader();
          db._executeTransaction({ 
            collections: { write: cn },
            action: function(params) {
              let db = require("internal").db;
              db[params.cn].insert({ _key: "meow", value: "abc" });
              db[params.cn].insert({ _key: "leader", value: "one" });
            },
            params: { cn }
          });
        },

        function(state) {
        },

        function(state) {
          assertEqual(2, db[cn].count());
          assertEqual("abc", db[cn].document("meow").value);

          assertNull(db[cn].firstExample({ _key: "follower" }));
          assertNotNull(db[cn].firstExample({ _key: "leader" }));
          let docs = db[cn].toArray();
          docs.sort(function(l, r) { 
            if (l._key !== r._key) {
              return l._key < r._key ? -1 : 1;
            }
            return 0;
          });
          assertEqual("leader", docs[0]._key);
          assertEqual("one", docs[0].value);
          checkCountConsistency(cn, 3);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test collection creation
    // //////////////////////////////////////////////////////////////////////////////

    testCreateCollection: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 100; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },

        function (state) {
        },

        function (state) {
          assertEqual(100, db._collection(cn).count());
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test collection dropping
    // //////////////////////////////////////////////////////////////////////////////

    testDropCollection: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 100; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
          db._drop(cn);
        },

        function (state) {
        },

        function (state) {
          assertNull(db._collection(cn));
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test index creation
    // //////////////////////////////////////////////////////////////////////////////

    testCreateIndex: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          db._create(cn);
          db._collection(cn).ensureIndex({
            type: 'persistent',
            fields: ['value']
          });
        },

        function (state) {
        },

        function (state) {
          let col = db._collection(cn);
          assertNotNull(col, 'collection does not exist');
          let idx = col.indexes();
          assertEqual(2, idx.length);
          assertEqual('primary', idx[0].type);
          assertEqual('persistent', idx[1].type);
          assertEqual(['value'], idx[1].fields);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test index dropping
    // //////////////////////////////////////////////////////////////////////////////

    testDropIndex: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          db._create(cn);
          let idx = db._collection(cn).ensureIndex({
            type: 'persistent',
            fields: ['value']
          });
          db._collection(cn).dropIndex(idx);
        },

        function (state) {
        },

        function (state) {
          let idx = db._collection(cn).indexes();
          assertEqual(1, idx.length);
          assertEqual('primary', idx[0].type);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test renaming
    // //////////////////////////////////////////////////////////////////////////////

    testRenameCollection: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          db._create(cn);
          db._collection(cn).rename(cn + 'Renamed');
        },

        function (state) {
        },

        function (state) {
          assertNull(db._collection(cn));
          assertNotNull(db._collection(cn + 'Renamed'));
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test renaming
    // //////////////////////////////////////////////////////////////////////////////

    testChangeCollection: function () {
      connectToLeader();

      compare(
        function (state) {
        },

        function (state) {
          db._create(cn);
          assertFalse(db._collection(cn).properties().waitForSync);
          db._collection(cn).properties({
            waitForSync: true
          });
        },

        function (state) {
        },

        function (state) {
          assertTrue(db._collection(cn).properties().waitForSync);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test truncating a small collection
    // //////////////////////////////////////////////////////////////////////////////

    testTruncateCollectionSmall: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 1000; i++) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },

        function (state) {
          // note: this truncate is executed as follows on the leader:
          // - begin trx
          // - remove doc1
          // - remove doc2
          // - ...
          // - remove docn
          // - commit
          db._collection(cn).truncate({ compact: false });
          
          // note: the following is necessary because otherwise the test
          // can fail. the reason is that the truncate above does produce
          // a separate tick value for the commit operation on the leader.
          // when the follower checks if it has fully applied the operations
          // from the leader, it will already think that is has applied all 
          // operations successfully after carrying out the last remove
          // operation. however, the changes are not yet committed, so there
          // is a race between the transaction committing on the follower
          // and the compare function checking the follower's apply
          // progress.
          // by adding a follow-up operation such as the following insert,
          // this race can be avoided.
          db._collection(cn).insert({});
        },

        function (state) {
        },

        function (state) {
          assertEqual(db._collection(cn).count(), 1);
          assertEqual(db._collection(cn).toArray().length, 1);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test truncating a bigger collection
    // //////////////////////////////////////////////////////////////////////////////

    testTruncateCollectionBigger: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < (32 * 1024 + 1); i++) {
            docs.push({ value: i });
            if (docs.length >= 1000) {
              c.insert(docs);
              docs = [];
            }
          }
          c.insert(docs);
        },

        function (state) {
          db._collection(cn).truncate(); // should hit range-delete in rocksdb
        },

        function (state) {
        },

        function (state) {
          assertEqual(db._collection(cn).count(), 0);
          assertEqual(db._collection(cn).all().toArray().length, 0);
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test long transaction, blocking
    // //////////////////////////////////////////////////////////////////////////////

    testLongTransactionBlocking: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function (params) {
              let wait = require('internal').wait;
              let db = require('internal').db;
              let c = db._collection(params.cn);

              for (let i = 0; i < 10; ++i) {
                c.insert({
                  test1: i,
                  type: 'longTransactionBlocking',
                  coll: 'UnitTestsReplication'
                });
                c.insert({
                  test2: i,
                  type: 'longTransactionBlocking',
                  coll: 'UnitTestsReplication'
                });

                // intentionally delay the transaction
                wait(0.75, false);
              }
            },
            params: {
              cn: cn
            }
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(20, state.count);
        },

        function (state) {
          // stop and restart replication on the follower
          assertTrue(replication.applier.state().state.running);
          replication.applier.stop();
          assertFalse(replication.applier.state().state.running);

          replication.applier.start();
          assertTrue(replication.applier.state().state.running);
        },

        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test long transaction, asynchronous
    // //////////////////////////////////////////////////////////////////////////////

    testLongTransactionAsync: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          let func = db._executeTransaction({
            collections: {
              write: cn
            },
            action: function (params) {
              let wait = require('internal').wait;
              let db = require('internal').db;
              let c = db._collection(params.cn);

              for (let i = 0; i < 10; ++i) {
                c.insert({
                  test1: i,
                  type: 'longTransactionAsync',
                  coll: 'UnitTestsReplication'
                });
                c.insert({
                  test2: i,
                  type: 'longTransactionAsync',
                  coll: 'UnitTestsReplication'
                });

                // intentionally delay the transaction
                wait(3.0, false);
              }
            },
            params: {
              cn: cn
            }
          });

          state.task = require('@arangodb/tasks').register({
            name: 'replication-test-async',
            command: String(func),
            params: {
              cn: cn
            }
          }).id;
        },

        function (state) {
          assertTrue(replication.applier.state().state.running);

          connectToLeader();
          try {
            require('@arangodb/tasks').get(state.task);
            // task exists
            connectToFollower();
            return 'wait';
          } catch (err) {
            // task does not exist. we're done
            state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;
            state.checksum = collectionChecksum(cn);
            state.count = collectionCount(cn);
            assertEqual(20, state.count);
            connectToFollower();
          }
        },

        function (state) {
          assertTrue(state.hasOwnProperty('count'));
          assertEqual(state.count, collectionCount(cn));
        }
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test long transaction, asynchronous
    // //////////////////////////////////////////////////////////////////////////////

    testLongTransactionAsyncWithFollowerRestarts: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
        },

        function (state) {
          let func = db._executeTransaction({
            collections: {
              write: cn
            },
            action: function (params) {
              let wait = require('internal').wait;
              let db = require('internal').db;
              let c = db._collection(params.cn);

              for (let i = 0; i < 10; ++i) {
                c.insert({
                  test1: i,
                  type: 'longTransactionAsyncWithFollowerRestarts',
                  coll: 'UnitTestsReplication'
                });
                c.insert({
                  test2: i,
                  type: 'longTransactionAsyncWithFollowerRestarts',
                  coll: 'UnitTestsReplication'
                });

                // intentionally delay the transaction
                wait(0.75, false);
              }
            },
            params: {
              cn: cn
            }
          });

          state.task = require('@arangodb/tasks').register({
            name: 'replication-test-async-with-restart',
            command: String(func),
            params: {
              cn: cn
            }
          }).id;
        },

        function (state) {
          // stop and restart replication on the follower
          assertTrue(replication.applier.state().state.running);
          replication.applier.stop();
          assertFalse(replication.applier.state().state.running);

          connectToLeader();
          try {
            require('@arangodb/tasks').get(state.task);
            // task exists
            connectToFollower();

            replication.applier.start();
            assertTrue(replication.applier.state().state.running);
            return 'wait';
          } catch (err) {
            // task does not exist anymore. we're done
            state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;
            state.checksum = collectionChecksum(cn);
            state.count = collectionCount(cn);
            assertEqual(20, state.count);
            connectToFollower();
            replication.applier.start();
            assertTrue(replication.applier.state().state.running);
          }
        },

        function (state) {
          assertEqual(state.count, collectionCount(cn));
        }
      );
    },
    
    testSearchAliasWithLinks: function () {
      connectToLeader();
      const idxName = "inverted_idx";

      compare(
        function (state) {
          let c = db._create(cn);
          let idx = c.ensureIndex({ type: "inverted", name: idxName, fields: [ { name: "value" } ] });
          let view = db._createView('UnitTestsSyncSearchAlias', 'search-alias', {
            indexes: [
              {
                collection: cn,
                index: idxName,
              }
            ]
          });
          assertEqual([{ collection: cn, index: idxName }], view.properties().indexes);
        },
        function () { },
        function () { },
        function (state) {
          let view = db._view('UnitTestsSyncSearchAlias');
          assertNotNull(view);
          assertEqual("search-alias", view.type());
          assertEqual([{ collection: cn, index: idxName }], view.properties().indexes);
        },
        {}
      );
    },
    
    testSearchAliasWithLinksAddedLater: function () {
      connectToLeader();
      const idxName = "inverted_idx";

      compare(
        function (state) {
          let c = db._create(cn);
          let idx = c.ensureIndex({ type: "inverted", name: idxName, fields: [ { name: "value" } ] });
          let view = db._createView('UnitTestsSyncSearchAlias', 'search-alias', {});
          assertEqual([], view.properties().indexes);
        },
        function () { 
          let view = db._view('UnitTestsSyncSearchAlias');
          view.properties({
            indexes: [
              {
                collection: cn,
                index: idxName,
              }
            ]
          });
          assertEqual([{ collection: cn, index: idxName }], view.properties().indexes);
        },
        function () { },
        function (state) {
          let view = db._view('UnitTestsSyncSearchAlias');
          assertNotNull(view);
          assertEqual("search-alias", view.type());
          let props = view.properties();
          assertEqual(1, props.indexes.length);
          assertEqual({ collection: cn, index: idxName }, props.indexes[0]);
        },
        {}
      );
    },

    testViewBasic: function () {
      connectToLeader();

      compare(
        function () { },
        function (state) {
          db._create(cn);
          let view = db._createView('UnitTestsSyncView', 'arangosearch', {});
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['text_en'] }
            }
          };
          view.properties({
            'links': links
          });
        },
        function () { },
        function (state) {
          let view = db._view('UnitTestsSyncView');
          assertNotNull(view);
          assertEqual("arangosearch", view.type());
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        {}
      );
    },

    testViewCreateWithLinks: function () {
      connectToLeader();

      compare(
        function () { },
        function (state) {
          db._create(cn);
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['text_en'] }
            }
          };
          db._createView('UnitTestsSyncView', 'arangosearch', { 'links': links });
        },
        function () { },
        function (state) {
          let view = db._view('UnitTestsSyncView');
          assertNotNull(view);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        {}
      );
    },
    
    testViewWithUpdateLater: function () {
      connectToLeader();

      compare(
        function () {
          db._create(cn);
          let view = db._createView("UnitTestsSyncView", "arangosearch", {});
          assertNotNull(view);
        },
        function (state) {
          let view = db._view('UnitTestsSyncView');
          assertNotNull(view);
          view.properties({
            "consolidationIntervalMsec": 42
          });
          assertEqual(42, view.properties().consolidationIntervalMsec);
        },
        function () { },
        function (state) {
          let view = db._view("UnitTestsSyncView");
          assertNotNull(view);
          assertEqual("arangosearch", view.type());
          let props = view.properties();
          assertEqual(props.consolidationIntervalMsec, 42);
        },
        {}
      );
    },
    
    testViewRename: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);
          let view = db._createView('UnitTestsSyncView', 'arangosearch', {});
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['text_en'] }
            }
          };
          view.properties({
            'links': links
          });
        },
        function (state) {
          // rename view on leader
          let view = db._view('UnitTestsSyncView');
          view.rename('UnitTestsSyncViewRenamed');
          view = db._view('UnitTestsSyncViewRenamed');
          assertNotNull(view);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        function (state) { },
        function (state) {
          let view = db._view('UnitTestsSyncViewRenamed');
          assertNotNull(view);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        {}
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test property update
    // //////////////////////////////////////////////////////////////////////////////
    testViewPropertiesUpdate: function () {
      connectToLeader();
      compare(
        function (state) { // leaderFunc1
          db._create(cn);
          db._createView(cn + 'View', 'arangosearch');
        },
        function (state) { // leaderFunc2
          let view = db._view(cn + 'View', 'arangosearch');
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['text_en'] }
            }
          };
          view.properties({
            links
          });
        },
        function () { // followerFuncOngoing
        }, // followerFuncOngoing
        function (state) { // followerFuncFinal
          let idx = db._collection(cn).indexes();
          assertEqual(1, idx.length); // primary

          let view = db._view(cn + 'View');
          assertNotNull(view);
          let props = view.properties();
          assertTrue(props.hasOwnProperty('links'));
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.links.hasOwnProperty(cn));
        });
    },

    testViewDrop: function () {
      connectToLeader();

      compare(
        function (state) {
          db._createView('UnitTestsSyncView', 'arangosearch', {});
        },
        function (state) {
          // rename view on leader
          let view = db._view('UnitTestsSyncView');
          view.drop();
        },
        function (state) { },
        function (state) {
          let view = db._view('UnitTestsSyncView');
          let x = 10;
          while (view && x-- > 0) {
            internal.sleep(1);
            db._flushCache();
            view = db._view('UnitTestsSyncView');
          }
          assertNull(view);
        },
        {}
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with views
    // //////////////////////////////////////////////////////////////////////////////

    testViewData: function () {
      connectToLeader();

      compare(
        function (state) { // leaderFunc1
          let c = db._create(cn);
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['text_en'] }
            }
          };
          let view = db._createView(cn + 'View', 'arangosearch', { links: links });
          assertEqual(Object.keys(view.properties().links).length, 1);

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              'value': i
            });
          }
          c.insert(docs);
        },
        function (state) { // leaderFunc2
          const txt = 'the red foxx jumps over the pond';
          db._collection(cn).insert({
            _key: 'testxxx',
            'value': -1,
            'text': txt
          });
          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5001, state.count);
        },
        function (state) { // followerFuncOngoing
          let view = db._view(cn + 'View');
          assertNotNull(view);
          let props = view.properties();
          assertTrue(props.hasOwnProperty('links'));
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.links.hasOwnProperty(cn));
        }, // followerFuncOngoing
        function (state) { // followerFuncFinal
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
          let idx = db._collection(cn).indexes();
          assertEqual(1, idx.length); // primary

          let view = db._view(cn + 'View');
          assertNotNull(view);
          let props = view.properties();
          assertTrue(props.hasOwnProperty('links'));
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.links.hasOwnProperty(cn));

          let res = db._query('FOR doc IN ' + view.name() + ' SEARCH doc.value >= 2500 OPTIONS { waitForSync: true } RETURN doc').toArray();
          assertEqual(2500, res.length);

          res = db._query('FOR doc IN ' + view.name() + ' SEARCH PHRASE(doc.text, "foxx jumps over", "text_en") OPTIONS { waitForSync: true } RETURN doc').toArray();
          assertEqual(1, res.length);
        });
    },
    
    testViewDataCustomAnalyzer: function () {
      connectToLeader();
      compare(
        function (state) { // leaderFunc1
          analyzers.save('custom', 'identity', {});
          let c = db._create(cn);
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: ['custom'] }
            }
          };
          let view = db._createView(cn + 'View', 'arangosearch', { links: links });
          assertEqual(Object.keys(view.properties().links).length, 1);

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              'value': i
            });
          }
          c.insert(docs);
        },
        function (state) { // leaderFunc2
          const txt = 'foxx';
          db._collection(cn).insert({
            _key: 'testxxx',
            'value': -1,
            'text': txt
          });
          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5001, state.count);
          analyzers.save('custom2', 'identity', {});
        },
        function (state) { // followerFuncOngoing
          let view = db._view(cn + 'View');
          assertNotNull(view);
          let props = view.properties();
          assertTrue(props.hasOwnProperty('links'));
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.links.hasOwnProperty(cn));
          // do not check results. We need to trigger analyzers cache reload
          db._query('FOR doc IN ' + view.name() + ' SEARCH PHRASE(doc.text, "foxx", "custom") '
                    + ' OPTIONS { waitForSync: true } RETURN doc').toArray();
        }, // followerFuncOngoing
        function (state) { // followerFuncFinal
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
          let idx = db._collection(cn).indexes();
          assertEqual(1, idx.length); // primary

          let view = db._view(cn + 'View');
          assertNotNull(view);
          let props = view.properties();
          assertTrue(props.hasOwnProperty('links'));
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.links.hasOwnProperty(cn));

          let res = db._query('FOR doc IN ' + view.name() + ' SEARCH doc.value >= 2500 OPTIONS { waitForSync: true } RETURN doc').toArray();
          assertEqual(2500, res.length);
          // analyzer here was added later so follower must properly reload its analyzers cache
          res = db._query('FOR doc IN ' + view.name() + ' SEARCH PHRASE(doc.text, TOKENS("foxx","custom2")[0], "custom") OPTIONS { waitForSync: true } RETURN doc').toArray();
          assertEqual(1, res.length);
        });
    }
  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite for _system
// //////////////////////////////////////////////////////////////////////////////

function ReplicationSuite () {
  'use strict';
  let suite = {

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief set up
    // //////////////////////////////////////////////////////////////////////////////

    setUp: function () {
      connectToFollower();
      try {
        replication.applier.stop();
        replication.applier.forget();
      }
      catch (err) {
      }

      connectToLeader();

      db._drop(cn);
      db._drop(cn2);
      db._drop(cn + 'Renamed');
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief tear down
    // //////////////////////////////////////////////////////////////////////////////

    tearDown: function () {
      connectToLeader();

      db._dropView('UnitTestsSyncView');
      db._dropView('UnitTestsSyncSearchAlias');
      db._dropView('UnitTestsSyncViewRenamed');
      db._dropView(cn + 'View');
      db._drop(cn);
      db._drop(cn2);
      db._analyzers.toArray().forEach(function(analyzer) {
        try { analyzers.remove(analyzer.name, true); } catch (err) {}
      });

      connectToFollower();
      replication.applier.stop();
      replication.applier.forget();

      db._dropView('UnitTestsSyncView');
      db._dropView('UnitTestsSyncSearchAlias');
      db._dropView('UnitTestsSyncViewRenamed');
      db._dropView(cn + 'View');
      db._drop(cn);
      db._drop(cn2);
      db._drop(cn + 'Renamed');
    }
  };
  deriveTestSuite(BaseTestConfig(), suite, '_Repl');
  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite for other database
// //////////////////////////////////////////////////////////////////////////////

function ReplicationOtherDBSuiteBase (dbName) {
  'use strict';

  // Setup documents to be stored on the leader.
  const n = 50;
  let docs = [];
  for (let i = 0; i < n; ++i) {
    docs.push({ value: i });
  }
  // Shared function that sets up replication
  // of the collection and inserts 50 documents.
  const setupReplication = function () {
    // Section - Leader
    connectToLeader();

    // Create the collection
    db._create(cn);

    // Section - Follower
    connectToFollower();

    // Setup Replication
    replication.applier.stop();
    replication.applier.forget();

    while (replication.applier.state().state.running) {
      internal.wait(0.1, false);
    }

    let config = {
      endpoint: leaderEndpoint,
      username: 'root',
      password: '',
      verbose: true,
      includeSystem: true
    };

    replication.setupReplication(config);

    // Section - Leader
    connectToLeader();
    // Insert some documents
    db._collection(cn).insert(docs);
    // Use counter as indicator
    let count = collectionCount(cn);
    assertEqual(n, count);

    // Section - Follower
    connectToFollower();
    
    // Give it some time to sync
    let tries = 20;
    while (tries-- > 0) {
      count = collectionCount(cn);
      if (count === n) {
        break;
      }
      internal.sleep(0.5);
    }

    // Now we should have the same amount of documents
    assertEqual(n, collectionCount(cn));
  };

  let suite = {
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief set up
    // //////////////////////////////////////////////////////////////////////////////

    setUp: function () {
      db._useDatabase('_system');
      connectToFollower();
      try {
        replication.applier.stop();
        replication.applier.forget();
      } catch (err) {
      }

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
      db._useDatabase('_system');

      connectToFollower();
      try {
        db._useDatabase(dbName);

        replication.applier.stop();
        replication.applier.forget();
      } catch (e) {
      }

      db._useDatabase('_system');
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }

      connectToLeader();

      db._useDatabase('_system');
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
    },
  };

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief test dropping a database on follower while replication is ongoing
  // //////////////////////////////////////////////////////////////////////////////
  suite["testDropDatabaseOnFollowerDuringReplication_" + dbName] = function () {
    setupReplication();

    // Section - Follower
    connectToFollower();

    // Now do the evil stuff: drop the database that is replicating right now.
    db._useDatabase('_system');

    // This shall not fail.
    db._dropDatabase(dbName);

    // Section - Leader
    connectToLeader();

    // Just write some more
    db._useDatabase(dbName);
    db._collection(cn).insert(docs);

    db._useDatabase('_system');

    // Section - Follower
    connectToFollower();

    // The DB should be gone and the server should be running.
    let dbs = db._databases();
    assertEqual(-1, dbs.indexOf(dbName));

    // We can setup everything here without problems.
    try {
      db._createDatabase(dbName);
    } catch (e) {
      assertFalse(true, 'Could not recreate database on follower: ' + e);
    }

    db._useDatabase(dbName);

    try {
      db._create(cn);
    } catch (e) {
      assertFalse(true, 'Could not recreate collection on follower: ' + e);
    }

    // Collection should be empty
    assertEqual(0, collectionCount(cn));

    // now test if the replication is actually
    // switched off

    // Section - Leader
    connectToLeader();
    // Insert some documents
    db._collection(cn).insert(docs);

    // Section - Follower
    connectToFollower();

    // be dumb and wait here until everything settles
    let tries = 100;
    while (replication.applier.state().state.running && tries-- > 0) {
      internal.wait(0.1, false);
    }

    // Now should still have empty collection
    assertEqual(0, collectionCount(cn));
      
    assertFalse(replication.applier.state().state.running);
  };

  suite["testDropDatabaseOnLeaderDuringReplication_" + dbName] = function () {
    setupReplication();

    // Section - Leader
    // Now do the evil stuff: drop the database that is replicating from right now.
    connectToLeader();
    db._useDatabase('_system');

    // This shall not fail.
    db._dropDatabase(dbName);

    // The DB should be gone and the server should be running.
    let dbs = db._databases();
    assertEqual(-1, dbs.indexOf(dbName));
    
    const lastLogTick = replication.logger.state().state.lastUncommittedLogTick;

    // Section - Follower
    connectToFollower();

    // Give it some time to sync (eventually, should not do anything...)
    let i = 30;
    while (i-- > 0) {
      let state = replication.applier.state();
      if (!state.running) {
        console.topic('replication=error', 'follower is not running');
        break;
      }
      if (compareTicks(state.lastAppliedContinuousTick, lastLogTick) >= 0 ||
          compareTicks(state.lastProcessedContinuousTick, lastLogTick) >= 0) {
        console.topic('replication=error', 'follower has caught up');
        break;
      }
      internal.sleep(0.5);
    }

    i = 60;
    while (i-- > 0) {
      let state = replication.applier.state();
      if (!state.running) {
        // all good
        return;
      }
      internal.sleep(0.5);
    }

    fail();
  };

  deriveTestSuite(BaseTestConfig(), suite, '_' + dbName);

  return suite;
}

function ReplicationOtherDBTraditionalNameSuite () {
  return ReplicationOtherDBSuiteBase('UnitTestDB');
}

function ReplicationOtherDBExtendedNameSuite () {
  return ReplicationOtherDBSuiteBase('Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨');
}

jsunity.run(ReplicationSuite);
jsunity.run(ReplicationOtherDBTraditionalNameSuite);
jsunity.run(ReplicationOtherDBExtendedNameSuite);

return jsunity.done();
