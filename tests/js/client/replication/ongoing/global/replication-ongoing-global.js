/* jshint globalstrict:false, strict:false, unused: false */
/* global assertEqual, assertTrue, assertFalse, assertNull, assertNotNull, arango, ARGUMENTS */

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
// / @author Michael Hackstein
// / @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const arangodb = require('@arangodb');
const db = arangodb.db;

const replication = require('@arangodb/replication');
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
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
  let c = db._collection(name).checksum(true, true);
  return c.checksum;
};

const collectionCount = function (name) {
  return db._collection(name).count();
};

const compare = function (leaderFunc, leaderFunc2, followerFuncOngoing, followerFuncFinal, applierConfiguration) {
  let state = {};

  db._flushCache();
  leaderFunc(state);

  connectToFollower();
  replication.globalApplier.stop();
  replication.globalApplier.forget();

  while (replication.globalApplier.state().state.running) {
    internal.wait(0.1, false);
  }

  applierConfiguration = applierConfiguration || {};
  applierConfiguration.endpoint = leaderEndpoint;
  applierConfiguration.username = 'root';
  applierConfiguration.password = '';
  applierConfiguration.includeSystem = false;
  applierConfiguration.requireFromPresent = true;

  let syncResult = replication.syncGlobal({
    endpoint: leaderEndpoint,
    username: 'root',
    password: '',
    verbose: true,
    includeSystem: false,
    keepBarrier: true,
    restrictType: applierConfiguration.restrictType,
    restrictCollections: applierConfiguration.restrictCollections
  });

  assertTrue(syncResult.hasOwnProperty('lastLogTick'));

  connectToLeader();
  leaderFunc2(state);

  // use lastLogTick as of now
  state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;

  if (!applierConfiguration.hasOwnProperty('chunkSize')) {
    applierConfiguration.chunkSize = 16384;
  }

  connectToFollower();

  replication.globalApplier.properties(applierConfiguration);
  replication.globalApplier.start(syncResult.lastLogTick, syncResult.barrierId);

  let printed = false;
  let handled = false;

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

    let followerState = replication.globalApplier.state();

    if (followerState.state.lastError.errorNum > 0) {
      console.topic('replication=error', 'follower has errored:', JSON.stringify(followerState.state.lastError));
      throw JSON.stringify(followerState.state.lastError);
    }

    if (!followerState.state.running) {
      console.topic('replication=error', 'follower is not running');
      break;
    }
    if (compareTicks(followerState.state.lastAppliedContinuousTick, state.lastLogTick) >= 0 ||
        compareTicks(followerState.state.lastProcessedContinuousTick, state.lastLogTick) >= 0) {
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
  followerFuncFinal(state);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief Base Test Config. Identitical part for _system and other DB
// //////////////////////////////////////////////////////////////////////////////

function BaseTestConfig () {
  'use strict';

  return {
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
          assertTrue(db._collection(cn).count() === 100);
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
          assertTrue(db._collection(cn).count() === 100);
          assertNull(db._collection(cn + '2'));
        },

        {
          restrictType: 'exclude',
          restrictCollections: [cn + '2']
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
          assertTrue(db._collection(cn).count() === 100);
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
    
    testTruncateCollectionBiggerAndThenSome: function () {
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
          assertEqual(db._collection(cn).count(), 0);
          assertEqual(db._collection(cn).toArray().length, 0);
          db._collection(cn).insert({_key: "a"});
          db._collection(cn).insert({_key: "b"});
        },

        function (state) {
        },

        function (state) {
          const c = db._collection(cn);
          assertEqual(c.count(), 2);
          assertEqual(c.toArray().length, 2);
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
          assertEqual(db._collection(cn).count(), 0);
          assertEqual(db._collection(cn).toArray().length, 0);
        },

        function (state) {
        },

        function (state) {
          const c = db._collection(cn);
          assertEqual(c.count(), 0);
          assertEqual(c.toArray().length, 0);
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
          assertTrue(replication.globalApplier.state().state.running);
          replication.globalApplier.stop();
          assertFalse(replication.globalApplier.state().state.running);

          replication.globalApplier.start();
          assertTrue(replication.globalApplier.state().state.running);
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
          assertTrue(replication.globalApplier.state().state.running);

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
          assertTrue(replication.globalApplier.state().state.running);
          replication.globalApplier.stop();
          assertFalse(replication.globalApplier.state().state.running);

          connectToLeader();
          try {
            require('@arangodb/tasks').get(state.task);
            // task exists
            connectToFollower();

            replication.globalApplier.start();
            assertTrue(replication.globalApplier.state().state.running);
            return 'wait';
          } catch (err) {
            // task does not exist anymore. we're done
            state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;
            state.checksum = collectionChecksum(cn);
            state.count = collectionCount(cn);
            assertEqual(20, state.count);
            connectToFollower();
            replication.globalApplier.start();
            assertTrue(replication.globalApplier.state().state.running);
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
          let props = view.properties();
          assertEqual(1, props.indexes.length);
          assertEqual({ collection: cn, index: idxName }, props.indexes[0]);
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
        function () {},
        function (state) {
          db._create(cn);
          let view = db._createView('UnitTestsSyncView', 'arangosearch', {});
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: { analyzers: [ 'text_en' ] }
            }
          };
          view.properties({
            'links': links
          });
        },
        function () {},
        function (state) {
          let view = db._view('UnitTestsSyncView');
          assertTrue(view !== null);
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
              text: {
                analyzers: [ 'text_en' ] }
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
          assertTrue(view !== null);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        function (state) {},
        function (state) {
          let view = db._view('UnitTestsSyncViewRenamed');
          assertTrue(view !== null);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        },
        {}
      );
    },

    testViewDrop: function () {
      connectToLeader();

      compare(
        function (state) {
          db._createView('UnitTestsSyncView', 'arangosearch', {});
        },
        function (state) {
          // drop view on leader
          let view = db._view('UnitTestsSyncView');
          view.drop();
        },
        function (state) {},
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
        replication.global.stop();
        replication.global.forget();
      } catch (err) {
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
      db._drop(cn);
      db._drop(cn2);

      connectToFollower();
      replication.globalApplier.stop();
      replication.globalApplier.forget();

      db._dropView('UnitTestsSyncView');
      db._dropView('UnitTestsSyncSearchAlias');
      db._dropView('UnitTestsSyncViewRenamed');
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

function ReplicationOtherDBSuite () {
  'use strict';
  const dbName = 'UnitTestDB';

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
    replication.globalApplier.stop();
    replication.globalApplier.forget();

    while (replication.globalApplier.state().state.running) {
      internal.wait(0.1, false);
    }

    let config = {
      endpoint: leaderEndpoint,
      username: 'root',
      password: '',
      verbose: true,
      includeSystem: false,
      restrictType: '',
      restrictCollections: [],
      keepBarrier: false
    };

    replication.setupReplicationGlobal(config);

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
    assertEqual(n, count);
  };

  let suite = {
    setUp: function () {
      db._useDatabase('_system');
      connectToFollower();
      try {
        replication.globalApplier.stop();
        replication.globalApplier.forget();
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

    tearDown: function () {
      db._useDatabase('_system');

      connectToFollower();
      db._useDatabase(dbName);

      replication.globalApplier.stop();
      replication.globalApplier.forget();

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

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test dropping a database on follower while replication is ongoing
    // //////////////////////////////////////////////////////////////////////////////

    testDropDatabaseOnFollowerDuringReplication: function () {
      setupReplication();

      // Section - Follower
      connectToFollower();

      assertTrue(replication.globalApplier.state().state.running);

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
      while (replication.globalApplier.state().state.running && tries-- > 0) {
        internal.wait(0.1, false);
      }

      // Now should still have empty collection
      assertEqual(0, collectionCount(cn));

      assertFalse(replication.globalApplier.state().state.running);
    },

    testDropDatabaseOnLeaderDuringReplication: function () {
      let waitUntil = function (cb) {
        let tries = 0;
        while (tries++ < 60 * 2) {
          if (cb()) {
            return;
          }
          internal.wait(0.5, false);
        }
        assertFalse(true, 'required condition not satisified: ' + String(cb));
      };

      setupReplication();

      db._useDatabase('_system');
      connectToFollower();
      // wait until database is present on follower as well
      waitUntil(function () { return (db._databases().indexOf(dbName) !== -1); });

      // Section - Leader
      // Now do the evil stuff: drop the database that is replicating from right now.
      connectToLeader();
      // This shall not fail.
      db._dropDatabase(dbName);

      db._useDatabase('_system');
      connectToFollower();
      waitUntil(function () { return (db._databases().indexOf(dbName) === -1); });

      // Now recreate a new database with this name
      connectToLeader();
      db._createDatabase(dbName);

      db._useDatabase(dbName);
      db._createDocumentCollection(cn);
      db._collection(cn).insert(docs);

      // Section - Follower
      db._useDatabase('_system');
      connectToFollower();
      waitUntil(function () { return (db._databases().indexOf(dbName) !== -1); });
      // database now present on follower

      // Now test if the Follower did replicate the new database...
      db._useDatabase(dbName);
      // wait for collection to appear
      waitUntil(function () {
        let cc = db._collection(cn);
        return cc !== null && cc.count() >= 50;
      });
      assertEqual(50, collectionCount(cn), 'The follower inserted the new collection data into the old one, it skipped the drop.');

      assertTrue(replication.globalApplier.state().state.running);
    },

    testSplitUpLargeTransactions: function () {
      // Section - Leader
      connectToLeader();

      // Create the collection
      db._create(cn);

      // Section - Follower
      connectToFollower();

      // Setup Replication
      replication.globalApplier.stop();
      replication.globalApplier.forget();

      while (replication.globalApplier.state().state.running) {
        internal.wait(0.1, false);
      }

      let config = {
        endpoint: leaderEndpoint,
        username: 'root',
        password: '',
        verbose: true,
        includeSystem: false,
        restrictType: '',
        restrictCollections: [],
        keepBarrier: false,
        chunkSize: 16384 // small chunksize should split up trxs
      };

      replication.setupReplicationGlobal(config);

      connectToLeader();

      let coll = db._collection(cn);
      const count = 100000;
      let docs = [];
      for (let i = 0; i < count; i++) {
        if (docs.length > 10000) {
          coll.insert(docs);
          docs = [];
        }
        docs.push({ value: i });
      }
      coll.insert(docs);

      // try to perform another operation afterwards
      const cn2 = cn + 'Test';
      db._create(cn2);

      let lastLogTick = replication.logger.state().state.lastUncommittedLogTick;

      // Section - Follower
      connectToFollower();

      let printed = false;
      while (true) {
        let followerState = replication.globalApplier.state();
        if (followerState.state.lastError.errorNum > 0) {
          console.topic('replication=error', 'follower has errored:', JSON.stringify(followerState.state.lastError));
          throw JSON.stringify(followerState.state.lastError);
        }

        if (!followerState.state.running) {
          console.topic('replication=error', 'follower is not running');
          break;
        }
        if (compareTicks(followerState.state.lastAppliedContinuousTick, lastLogTick) >= 0 ||
            compareTicks(followerState.state.lastProcessedContinuousTick, lastLogTick) >= 0) {
          console.topic('replication=debug',
                        'follower has caught up. state.lastLogTick:', followerState.state.lastLogTick,
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

      // Now we should have the same amount of documents
      assertEqual(count, collectionCount(cn));
      assertNotNull(db._collection(cn2));
      assertTrue(replication.globalApplier.state().state.running);
    }
  };

  deriveTestSuite(BaseTestConfig(), suite, '_ReplOther');

  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

jsunity.run(ReplicationSuite);
jsunity.run(ReplicationOtherDBSuite);

// TODO Add test for:
// Accessing globalApplier in non system database.
// Try to setup global repliaction in non system database.

return jsunity.done();
