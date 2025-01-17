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
  reconnectRetry(leaderEndpoint, db._name(), replicatorUser, replicatorPassword);
};

const connectToFollower = function() {
  reconnectRetry(followerEndpoint, db._name(), 'root', '');
};

const collectionChecksum = function(name) {
  var c = db._collection(name).checksum(true, true);
  return c.checksum;
};

const collectionCount = function(name) {
  return db._collection(name).count();
};

const waitForSync = function() {
  let state = {};
  let printed = false;
  connectToLeader();
  state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;
  IM.arangods[1].connect();
  while (true) {
    connectToFollower();
    var followerState = replication.applier.state();
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
};

const compare = function(leaderFunc, followerFunc, applierConfiguration) {
  var state = {};

  db._flushCache();
  leaderFunc(state);

  connectToFollower();
  replication.applier.stop();

  while (replication.applier.state().state.running) {
    internal.wait(0.1, false);
  }

  var includeSystem = true;
  var restrictType = '';
  var restrictCollections = [];

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

  var syncResult = replication.sync({
    endpoint: leaderEndpoint,
    username: replicatorUser,
    password: replicatorPassword,
    verbose: true,
    includeSystem: includeSystem,
    restrictType: restrictType,
    restrictCollections: restrictCollections,
    waitForSyncTimeout: 120,
    keepBarrier: true
  });

  db._flushCache();
  followerFunc(state);

  assertTrue(syncResult.hasOwnProperty('lastLogTick'));

  applierConfiguration = applierConfiguration || {};
  applierConfiguration.endpoint = leaderEndpoint;
  applierConfiguration.username = replicatorUser;
  applierConfiguration.password = replicatorPassword;
  applierConfiguration.includeSystem = includeSystem;
  applierConfiguration.requireFromPresent = true;

  if (!applierConfiguration.hasOwnProperty('chunkSize')) {
    applierConfiguration.chunkSize = 16384;
  }

  replication.applier.properties(applierConfiguration);
  replication.applier.start(syncResult.lastLogTick, syncResult.barrierId);

  var printed = false;

  while (true) {
    var followerState = replication.applier.state();

    if (followerState.state.lastError.errorNum > 0) {
      console.warn('follower has errored:', JSON.stringify(followerState.state.lastError));
      break;
    }

    if (!followerState.state.running) {
      console.warn('follower is not running');
      break;
    }

    if (compareTicks(followerState.state.lastAppliedContinuousTick, syncResult.lastLogTick) >= 0 ||
      compareTicks(followerState.state.lastProcessedContinuousTick, syncResult.lastLogTick) >= 0) {
      console.debug('follower has caught up. syncResult.lastLogTick:', syncResult.lastLogTick, 'followerState.lastAppliedContinuousTick:', followerState.state.lastAppliedContinuousTick, 'followerState.lastProcessedContinuousTick:', followerState.state.lastProcessedContinuousTick, 'followerState:', followerState);
      break;
    }

    if (!printed) {
      console.debug('waiting for follower to catch up');
      printed = true;
    }
    internal.wait(0.5, false);
  }

  db._flushCache();
  followerFunc(state);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief Base Test Config. Identitical part for _system and other DB
// //////////////////////////////////////////////////////////////////////////////

function BaseTestConfig() {
  'use strict';

  return {
    tearDownAll: function() {
      db._flushCache();
      db._users.toArray().forEach(user => {
        if (user.user !== "root" && user.user !== "replicator-user") {
          userManager.remove(user.user);
        }
      });
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test invalid credentials
    // /////////////////////////////////////////////////////////////////////////////

    testInvalidCredentials1: function() {
      var configuration = {
        endpoint: leaderEndpoint,
        username: replicatorUser,
        password: replicatorPassword + 'xx' // invalid
      };

      try {
        replication.applier.properties(configuration);
      } catch (err) {
        assertEqual(errors.ERROR_HTTP_UNAUTHORIZED.code, err.errorNum);
      }
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test invalid credentials
    // /////////////////////////////////////////////////////////////////////////////

    testInvalidCredentials2: function() {
      var configuration = {
        endpoint: leaderEndpoint,
        username: replicatorUser + 'xx', // invalid
        password: replicatorPassword
      };

      try {
        replication.applier.properties(configuration);
      } catch (err) {
        assertEqual(errors.ERROR_HTTP_UNAUTHORIZED.code, err.errorNum);
      }
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test invalid credentials
    // /////////////////////////////////////////////////////////////////////////////

    testInvalidCredentials3: function() {
      var configuration = {
        endpoint: leaderEndpoint,
        username: 'root',
        password: 'abc'
      };

      try {
        replication.applier.properties(configuration);
      } catch (err) {
        assertEqual(errors.ERROR_HTTP_UNAUTHORIZED.code, err.errorNum);
      }
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test trx with multiple collections
    // /////////////////////////////////////////////////////////////////////////////

    testTrxMultiCollections: function() {
      connectToLeader();

      compare(
        function() {
          db._create(cn);
          db._create(cn2);

          db[cn].insert({
            _key: 'foo',
            value: 1
          });
          db[cn2].insert({
            _key: 'bar',
            value: 'A'
          });

          db._executeTransaction({
            collections: {
              write: [cn, cn2]
            },
            action: function(params) {
              var c = require('internal').db._collection(params.cn);
              var c2 = require('internal').db._collection(params.cn2);

              c.replace('foo', {
                value: 2
              });
              c.insert({
                _key: 'foo2',
                value: 3
              });

              c2.replace('bar', {
                value: 'B'
              });
              c2.insert({
                _key: 'bar2',
                value: 'C'
              });
            },
            params: {
              cn,
              cn2
            }
          });
        },
        function() {
          assertEqual(2, db[cn].count());
          assertEqual(2, db[cn].document('foo').value);
          assertEqual(3, db[cn].document('foo2').value);

          assertEqual(2, db[cn2].count());
          assertEqual('B', db[cn2].document('bar').value);
          assertEqual('C', db[cn2].document('bar2').value);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test few documents
    // /////////////////////////////////////////////////////////////////////////////

    testFew: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({value: i});
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
      // shoot me again, I ain't dead yet.
      waitForSync();
      db._drop(cn);
      waitForSync();
      internal.sleep(10);
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test many documents
    // /////////////////////////////////////////////////////////////////////////////

    testMany: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 50000; ++i) {
            docs.push({value: i});
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(50000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test many documents
    // /////////////////////////////////////////////////////////////////////////////

    testManyMore: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 150000; ++i) {
            docs.push({value: i});
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(150000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test big markers
    // /////////////////////////////////////////////////////////////////////////////

    testBigMarkersObject: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);

          var doc = {};
          for (let i = 0; i < 1000; ++i) {
            doc['test' + i] = 'the quick brown foxx jumped over the LAZY dog';
          }

          for (let i = 0; i < 100; ++i) {
            c.save({
              'value': i,
              'values': doc
            });
          }

          var d = c.any();
          assertEqual(1000, Object.keys(d.values).length);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(100, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test big markers
    // /////////////////////////////////////////////////////////////////////////////

    testBigMarkersArray: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);
          let doc = [];

          for (let i = 0; i < 1000; ++i) {
            doc.push('the quick brown foxx jumped over the LAZY dog');
          }

          for (let i = 0; i < 100; ++i) {
            c.save({
              'value': i,
              'values': doc
            });
          }

          var d = c.any();
          assertEqual(1000, d.values.length);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(100, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test heterogenous markers
    // /////////////////////////////////////////////////////////////////////////////

    testHeterogenousMarkers: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);
          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            let doc = {};
            doc['test' + i] = 'the quick brown foxx jumped over the LAZY dog';
            docs.push({
              'value': i,
              'values': doc
            });
          }
          c.insert(docs);

          var d = c.any();
          assertEqual(1, Object.keys(d.values).length);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test empty markers
    // /////////////////////////////////////////////////////////////////////////////

    testEmptyMarkers: function() {
      connectToLeader();

      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({});
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test documents
    // /////////////////////////////////////////////////////////////////////////////

    testDocuments1: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              'value': i,
              'foo': true,
              'bar': [i, false],
              'value2': null,
              'mydata': {
                'test': ['abc', 'def']
              }
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test documents
    // /////////////////////////////////////////////////////////////////////////////

    testDocuments2: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          for (let i = 0; i < 1000; ++i) {
            c.save({
              'abc': true,
              '_key': 'test' + i
            });
            if (i % 3 === 0) {
              c.remove('test' + i);
            } else if (i % 5 === 0) {
              c.update('test' + i, {
                'def': 'hifh'
              });
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(666, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test documents
    // /////////////////////////////////////////////////////////////////////////////

    testDocuments3: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 50000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'foo': 'bar',
              'baz': 'bat'
            });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(50000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test documents
    // /////////////////////////////////////////////////////////////////////////////

    testDocuments4: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 50000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'foo': 'bar',
              'baz': 'bat'
            });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(50000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }, {
          chunkSize: 512
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test documents
    // /////////////////////////////////////////////////////////////////////////////

    testDocuments5: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          for (let i = 0; i < 100; ++i) {
            c.save({
              'abc': true,
              '_key': 'test' + i
            });
            if (i % 3 === 0) {
              c.remove('test' + i);
            } else if (i % 5 === 0) {
              c.update('test' + i, {
                'def': 'hifh'
              });
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(66, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test edges
    // /////////////////////////////////////////////////////////////////////////////

    testEdges: function() {
      compare(
        function(state) {
          let v = db._create(cn);
          let e = db._createEdgeCollection(cn2);

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i
            });
          }
          v.save(docs);

          docs = [];
          for (let i = 0; i < 5000; i += 10) {
            docs.push({
              _from: cn + '/test' + i,
              _to: cn + '/test' + i,
              'foo': 'bar',
              'value': i
            });
          }
          e.save(docs);

          state.checksum1 = collectionChecksum(cn);
          state.count1 = collectionCount(cn);
          assertEqual(1000, state.count1);

          state.checksum2 = collectionChecksum(cn2);
          state.count2 = collectionCount(cn2);
          assertEqual(500, state.count2);
        },
        function(state) {
          assertEqual(state.count1, collectionCount(cn));
          assertEqual(state.checksum1, collectionChecksum(cn));

          assertEqual(state.count2, collectionCount(cn2));
          assertEqual(state.checksum2, collectionChecksum(cn2));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionInsert: function() {
      compare(
        function(state) {
          db._create(cn);

          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function(params) {
              var c = require('internal').db._collection(params.cn);

              for (var i = 0; i < 1000; ++i) {
                c.save({
                  '_key': 'test' + i
                });
              }
            },
            params: {cn}
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionInsertFail: function() {
      compare(
        function(state) {
          db._create(cn);

          try {
            db._executeTransaction({
              collections: {
                write: cn
              },
              action: function(params) {
                var c = require('@arangodb').db._collection(params.cn);
                for (var i = 0; i < 1000; ++i) {
                  c.save({
                    '_key': 'test' + i
                  });
                }

                throw new Error('rollback!');
              },
              params: {cn}
            });
            fail();
          } catch (err) {
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(0, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionRemove: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i
            });
          }
          c.insert(docs);

          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function(params) {
              var c = require('@arangodb').db._collection(params.cn);
              for (var i = 0; i < 1000; ++i) {
                c.remove('test' + i);
              }
            },
            params: {cn}
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(0, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionRemoveFail: function() {
      compare(
        function(state) {
          let c = db._create(cn);

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i
            });
          }
          c.insert(docs);

          try {
            db._executeTransaction({
              collections: {
                write: cn
              },
              action: function(params) {
                var c = require('@arangodb').db._collection(params.cn);
                for (var i = 0; i < 1000; ++i) {
                  c.remove('test' + i);
                }
                throw new Error('peng!');
              },
              params: {cn}
            });
          } catch (err) {

          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionMultiOps: function() {
      compare(
        function(state) {
          db._create(cn);

          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function(params) {
              var c = require('internal').db._collection(params.cn);
              var i;

              for (i = 0; i < 1000; ++i) {
                c.save({
                  '_key': 'test' + i
                });
              }

              for (i = 0; i < 1000; ++i) {
                c.update('test' + i, {
                  'foo': 'bar' + i
                });
              }

              for (i = 0; i < 1000; ++i) {
                c.update('test' + i, {
                  'foo': 'baz' + i
                });
              }

              for (i = 0; i < 1000; i += 10) {
                c.remove('test' + i);
              }
            },
            params: {
              'cn': cn
            }
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(900, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test big transaction
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionBig: function() {
      compare(
        function(state) {
          db._create(cn);

          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function(params) {
              var c = require('internal').db._collection(params.cn);
              var i;

              for (i = 0; i < 50000; ++i) {
                c.save({
                  '_key': 'test' + i,
                  value: i
                });
                c.update('test' + i, {
                  value: i + 1
                });

                if (i % 5 === 0) {
                  c.remove('test' + i);
                }
              }
            },
            params: {
              'cn': cn
            }
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(40000, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }, {
          chunkSize: 2048
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test delayed transaction
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionDelayed: function() {
      compare(
        function(state) {
          db._create(cn);

          db._executeTransaction({
            collections: {
              write: cn
            },
            action: function(params) {
              var c = require('internal').db._collection(params.cn);
              var i;
              var wait = require('internal').wait;

              for (i = 0; i < 10; ++i) {
                c.save({
                  '_key': 'test' + i,
                  value: i
                });
                c.update('test' + i, {
                  value: i + 1
                });

                wait(1, false);
              }
            },
            params: {
              'cn': cn
            }
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(10, state.count);
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        }, {
          chunkSize: 2048
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionMulti: function() {
      compare(
        function(state) {
          db._create(cn);
          db._create(cn2);

          db._executeTransaction({
            collections: {
              write: [cn, cn2]
            },
            action: function(params) {
              var c1 = require('internal').db._collection(params.cn);
              var c2 = require('internal').db._collection(params.cn2);
              var i;

              for (i = 0; i < 1000; ++i) {
                c1.save({
                  '_key': 'test' + i
                });
                c2.save({
                  '_key': 'test' + i,
                  'foo': 'bar'
                });
              }

              for (i = 0; i < 1000; ++i) {
                c1.update('test' + i, {
                  'foo': 'bar' + i
                });
              }

              for (i = 0; i < 1000; ++i) {
                c1.update('test' + i, {
                  'foo': 'baz' + i
                });
                c2.update('test' + i, {
                  'foo': 'baz' + i
                });
              }

              for (i = 0; i < 1000; i += 10) {
                c1.remove('test' + i);
                c2.remove('test' + i);
              }
            },
            params: {
              'cn': cn,
              'cn2': cn2
            }
          });

          state.checksum1 = collectionChecksum(cn);
          state.checksum2 = collectionChecksum(cn2);
          state.count1 = collectionCount(cn);
          state.count2 = collectionCount(cn2);
          assertEqual(900, state.count1);
          assertEqual(900, state.count2);
        },
        function(state) {
          assertEqual(state.count1, collectionCount(cn));
          assertEqual(state.count2, collectionCount(cn2));
          assertEqual(state.checksum1, collectionChecksum(cn));
          assertEqual(state.checksum2, collectionChecksum(cn2));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test transactions
    // /////////////////////////////////////////////////////////////////////////////

    testTransactionAbort: function() {
      compare(
        function(state) {
          db._create(cn);
          db._create(cn2);

          db._collection(cn).save({
            foo: 'bar'
          });
          db._collection(cn2).save({
            bar: 'baz'
          });

          try {
            db._executeTransaction({
              collections: {
                write: [cn, cn2]
              },
              action: function(params) {
                var c1 = require('internal').db._collection(params.cn);
                var c2 = require('internal').db._collection(params.cn2);
                var i;

                for (i = 0; i < 1000; ++i) {
                  c1.save({
                    '_key': 'test' + i
                  });
                  c2.save({
                    '_key': 'test' + i,
                    'foo': 'bar'
                  });
                }

                for (i = 0; i < 1000; ++i) {
                  c1.update('test' + i, {
                    'foo': 'bar' + i
                  });
                }

                for (i = 0; i < 1000; ++i) {
                  c1.update('test' + i, {
                    'foo': 'baz' + i
                  });
                  c2.update('test' + i, {
                    'foo': 'baz' + i
                  });
                }

                for (i = 0; i < 1000; i += 10) {
                  c1.remove('test' + i);
                  c2.remove('test' + i);
                }

                throw new Error('rollback!');
              },
              params: {
                'cn': cn,
                'cn2': cn2
              }
            });
            fail();
          } catch (err) {
          }

          state.checksum1 = collectionChecksum(cn);
          state.checksum2 = collectionChecksum(cn2);
          state.count1 = collectionCount(cn);
          state.count2 = collectionCount(cn2);
          assertEqual(1, state.count1);
          assertEqual(1, state.count2);
        },
        function(state) {
          assertEqual(state.count1, collectionCount(cn));
          assertEqual(state.count2, collectionCount(cn2));
          assertEqual(state.checksum1, collectionChecksum(cn));
          assertEqual(state.checksum2, collectionChecksum(cn2));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test rename collection
    // /////////////////////////////////////////////////////////////////////////////

    testRenameCollection1: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            waitForSync: false,
            keyOptions: {
              allowUserKeys: false
            }
          });

          c.rename(cn2);

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          try {
            db._collection(cn).properties();
            fail();
          } catch (err) {
            // original collection was renamed
          }

          var properties = db._collection(cn2).properties();
          assertEqual(cn2, db._collection(cn2).name());
          assertFalse(properties.waitForSync);
          assertFalse(properties.deleted);
          assertFalse(properties.keyOptions.allowUserKeys);
          assertEqual('traditional', properties.keyOptions.type);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test rename collection
    // /////////////////////////////////////////////////////////////////////////////

    testRenameCollection2: function() {
      compare(
        function(state) {
          var c = db._create(cn);
          c.rename(cn2);
          c.rename(cn);

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          try {
            db._collection(cn2).properties();
            fail();
          } catch (err) {
            // collection was renamed
          }

          assertEqual(cn, db._collection(cn).name());
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test change collection
    // /////////////////////////////////////////////////////////////////////////////

    testChangeCollection1: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            waitForSync: false,
          });

          var properties = c.properties();
          assertFalse(properties.waitForSync);

          properties = c.properties({
            waitForSync: true,
          });
          assertTrue(properties.waitForSync);

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertTrue(properties.waitForSync);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test change collection
    // /////////////////////////////////////////////////////////////////////////////

    testChangeCollection2: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            waitForSync: true,
          });

          var properties = c.properties();
          assertTrue(properties.waitForSync);

          properties = c.properties({
            waitForSync: false,
          });
          assertFalse(properties.waitForSync);

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertFalse(properties.waitForSync);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test create collection
    // /////////////////////////////////////////////////////////////////////////////

    testCreateCollection1: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            waitForSync: false,
          });

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertFalse(properties.waitForSync);
          assertFalse(properties.deleted);
          assertTrue(properties.keyOptions.allowUserKeys);
          assertEqual('traditional', properties.keyOptions.type);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test create collection
    // /////////////////////////////////////////////////////////////////////////////

    testCreateCollection2: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            keyOptions: {
              type: 'autoincrement',
              allowUserKeys: false
            },
            waitForSync: true,
          });

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertTrue(properties.waitForSync);
          assertFalse(properties.deleted);
          assertFalse(properties.keyOptions.allowUserKeys);
          assertEqual('autoincrement', properties.keyOptions.type);
        }
      );
    },

    testCreateCollectionKeygenUuid: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            keyOptions: {
              type: 'uuid',
              allowUserKeys: false
            }
          });

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertFalse(properties.keyOptions.allowUserKeys);
          assertEqual('uuid', properties.keyOptions.type);
        }
      );
    },

    testCreateCollectionKeygenPadded: function() {
      compare(
        function(state) {
          var c = db._create(cn, {
            keyOptions: {
              type: 'padded',
              allowUserKeys: false
            }
          });

          state.cid = c._id;
          state.properties = c.properties();
        },
        function(state) {
          var properties = db._collection(cn).properties();
          assertEqual(cn, db._collection(cn).name());
          assertFalse(properties.keyOptions.allowUserKeys);
          assertEqual('padded', properties.keyOptions.type);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test drop collection
    // /////////////////////////////////////////////////////////////////////////////

    testDropCollection: function() {
      compare(
        function(state) {
          var c = db._create(cn);
          c.drop();
        },
        function(state) {
          assertNull(db._collection(cn));
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test hash index
    // /////////////////////////////////////////////////////////////////////////////

    testHashIndex: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "hash", fields: ["a", "b"]});

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'a': parseInt(i / 2),
              'b': i
            });
          }
          c.insert(docs);
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1001, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('hash', state.idx.type);
          assertFalse(state.idx.unique);
          assertFalse(state.idx.sparse);
          assertEqual(['a', 'b'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test hash index
    // /////////////////////////////////////////////////////////////////////////////

    testSparseHashIndex: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "hash", fields: ["a", "b"], sparse: true});

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'a': parseInt(i / 2),
              'b': i
            });
          }
          c.insert(docs);
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1001, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('hash', state.idx.type);
          assertFalse(state.idx.unique);
          assertTrue(state.idx.sparse);
          assertEqual(['a', 'b'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test unique constraint
    // /////////////////////////////////////////////////////////////////////////////

    testUniqueConstraint: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "hash", fields: ["a"], unique: true});

          for (let i = 0; i < 1000; ++i) {
            try {
              c.save({
                '_key': 'test' + i,
                'a': parseInt(i / 2)
              });
            } catch (err) {
            }
          }
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(501, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('hash', state.idx.type);
          assertTrue(state.idx.unique);
          assertFalse(state.idx.sparse);
          assertEqual(['a'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test unique constraint
    // /////////////////////////////////////////////////////////////////////////////

    testSparseUniqueConstraint: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "hash", fields: ["a"], unique: true, sparse: true});

          for (let i = 0; i < 1000; ++i) {
            try {
              c.save({
                '_key': 'test' + i,
                'a': parseInt(i / 2)
              });
            } catch (err) {
            }
          }
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(501, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('hash', state.idx.type);
          assertTrue(state.idx.unique);
          assertTrue(state.idx.sparse);
          assertEqual(['a'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test skiplist
    // /////////////////////////////////////////////////////////////////////////////

    testSkiplist: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "skiplist", fields: ["a", "b"]});

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'a': parseInt(i / 2),
              'b': i
            });
          }
          c.insert(docs);
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1001, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('skiplist', state.idx.type);
          assertFalse(state.idx.unique);
          assertFalse(state.idx.sparse);
          assertEqual(['a', 'b'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test skiplist
    // /////////////////////////////////////////////////////////////////////////////

    testSparseSkiplist: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "skiplist", fields: ["a", "b"], sparse: true});

          let docs = [];
          for (let i = 0; i < 1000; ++i) {
            docs.push({
              '_key': 'test' + i,
              'a': parseInt(i / 2),
              'b': i
            });
          }
          docs.push({});
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(1001, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('skiplist', state.idx.type);
          assertFalse(state.idx.unique);
          assertTrue(state.idx.sparse);
          assertEqual(['a', 'b'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test unique skiplist
    // /////////////////////////////////////////////////////////////////////////////

    testUniqueSkiplist: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "skiplist", fields: ["a"], unique: true});

          for (let i = 0; i < 1000; ++i) {
            try {
              c.save({
                '_key': 'test' + i,
                'a': parseInt(i / 2)
              });
            } catch (err) {
            }
          }
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(501, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('skiplist', state.idx.type);
          assertTrue(state.idx.unique);
          assertFalse(state.idx.sparse);
          assertEqual(['a'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test unique skiplist
    // /////////////////////////////////////////////////////////////////////////////

    testUniqueSparseSkiplist: function() {
      compare(
        function(state) {
          let c = db._create(cn);
          c.ensureIndex({type: "skiplist", fields: ["a"], unique: true, sparse: true});

          for (let i = 0; i < 1000; ++i) {
            try {
              c.save({
                '_key': 'test' + i,
                'a': parseInt(i / 2)
              });
            } catch (err) {
            }
          }
          c.save({});

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(501, state.count);

          state.idx = c.indexes()[1];
        },
        function(state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes()[1];
          assertEqual(state.idx.id, idx.id);
          assertEqual('skiplist', state.idx.type);
          assertTrue(state.idx.unique);
          assertTrue(state.idx.sparse);
          assertEqual(['a'], state.idx.fields);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test system collection
    // /////////////////////////////////////////////////////////////////////////////

    testSystemCollectionWithDefaults: function() {
      compare(
        function(state) {
          var c = db._create(systemCn, {
            isSystem: true
          });
          c.save({
            _key: 'UnitTester',
            testValue: 42
          });
        },
        function(state) {
          var doc = db[systemCn].document('UnitTester');
          assertEqual(42, doc.testValue);
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test system collection
    // /////////////////////////////////////////////////////////////////////////////

    testSystemCollectionExcludeSystem: function() {
      compare(
        function(state) {
          var c = db._create(systemCn, {
            isSystem: true
          });
          c.save({
            _key: 'UnitTester',
            testValue: 42
          });
        },
        function(state) {
          assertNull(db._collection(systemCn));
        }, {
          includeSystem: false
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test system collection
    // /////////////////////////////////////////////////////////////////////////////

    testSystemCollectionExcludeCollection: function() {
      compare(
        function(state) {
          var c = db._create(systemCn, {
            isSystem: true
          });
          c.save({
            _key: 'UnitTester',
            testValue: 42
          });
        },
        function(state) {
          assertNull(db._collection(systemCn));
        }, {
          includeSystem: true,
          restrictType: 'exclude',
          restrictCollections: [systemCn]
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test system collection
    // /////////////////////////////////////////////////////////////////////////////

    testSystemCollectionIncludeCollection: function() {
      compare(
        function(state) {
          var c = db._create(systemCn, {
            isSystem: true
          });
          c.save({
            _key: 'UnitTester',
            testValue: 42
          });
        },
        function(state) {
          var doc = db[systemCn].document('UnitTester');
          assertEqual(42, doc.testValue);
        }, {
          includeSystem: true,
          restrictType: 'include',
          restrictCollections: [systemCn]
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test include/exclude collection
    // /////////////////////////////////////////////////////////////////////////////

    testCollectionIncludeCollection: function() {
      compare(
        function(state) {
          var c1 = db._create(cn);
          var c2 = db._create(cn2);
          c1.save({
            _key: 'UnitTester',
            testValue: 42
          });
          c2.save({
            _key: 'UnitTester',
            testValue: 23
          });
        },
        function(state) {
          var doc = db[cn].document('UnitTester');
          assertEqual(42, doc.testValue);
          assertNull(db._collection(cn2));
        }, {
          restrictType: 'include',
          restrictCollections: [cn]
        }
      );
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief test include/exclude collection
    // /////////////////////////////////////////////////////////////////////////////

    testCollectionExcludeCollection: function() {
      compare(
        function(state) {
          var c1 = db._create(cn);
          var c2 = db._create(cn2);
          c1.save({
            _key: 'UnitTester',
            testValue: 42
          });
          c2.save({
            _key: 'UnitTester',
            testValue: 23
          });
        },
        function(state) {
          var doc = db[cn].document('UnitTester');
          assertEqual(42, doc.testValue);
          assertNull(db._collection(cn2));
        }, {
          restrictType: 'exclude',
          restrictCollections: [cn2]
        }
      );
    },
    
    testViewSearchAlias: function() {
      const idxName = "inverted_idx";
      compare(
        function(state) {
          let c = db._create(cn);
          let idx = c.ensureIndex({ type: "inverted", name: idxName, fields: [ { name: "value" } ] });
          let view = db._createView('UnitTestsSyncSearchAlias', 'search-alias', {});
          view.properties({
            indexes: [
              {
                collection: cn,
                index: idxName
              }
            ]
          });
          assertEqual(1, view.properties().indexes.length);
        },
        function(state) {
          let view = db._view('UnitTestsSyncSearchAlias');
          assertNotNull(view);
          assertEqual("search-alias", view.type());
          let props = view.properties();
          assertEqual(1, props.indexes.length);
          assertEqual({ collection: cn, index: idxName }, props.indexes[0]);
        }
      );
    },

    testViewBasic: function() {
      compare(
        function(state) {
          db._create(cn);
          let view = db._createView('UnitTestsSyncView', 'arangosearch', {});
          let links = {};
          links[cn] = {
            includeAllFields: true,
            fields: {
              text: {analyzers: ['text_en']}
            }
          };
          view.properties({'links': links});
        },
        function(state) {
          let view = db._view('UnitTestsSyncView');
          assertNotNull(view);
          let props = view.properties();
          assertEqual(Object.keys(props.links).length, 1);
          assertTrue(props.hasOwnProperty('links'));
          assertTrue(props.links.hasOwnProperty(cn));
        }
      );
    },

    testTailingWithTooHighSequenceNumber: function() {
      connectToLeader();

      const dbPrefix = db._name() === '_system' ? '' : '/_db/' + encodeURIComponent(db._name());

      const {
        lastTick: snapshotTick,
        id: replicationContextId
      } = arango.POST(`${dbPrefix}/_api/replication/batch?syncerId=123`, {ttl: 120});

      const callWailTail = (tick) => {
        const result = arango.GET_RAW(`${dbPrefix}/_api/wal/tail?from=${tick}&syncerId=123`);
        assertFalse(result.error, `Expected call to succeed, but got ${JSON.stringify(result)}`);
        assertEqual(204, result.code, `Unexpected response ${JSON.stringify(result)}`);
        return result;
      };

      try {
        // use a too high sequence number
        let result = callWailTail(snapshotTick * 100000);
        assertEqual("false", result.headers["x-arango-replication-checkmore"]);
        assertEqual("0", result.headers["x-arango-replication-lastincluded"]);
        let lastScanned = result.headers["x-arango-replication-lastscanned"];
        assertTrue(lastScanned === "0" || lastScanned === result.headers["x-arango-replication-lasttick"]);
      } finally {
        arango.DELETE(`${dbPrefix}/_api/replication/batch/${replicationContextId}`);
      }
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief Check that different syncer IDs and their WAL ticks are tracked
    //         separately
    // /////////////////////////////////////////////////////////////////////////////

    testWalRetain: function() {
      connectToLeader();

      const dbPrefix = db._name() === '_system' ? '' : '/_db/' + encodeURIComponent(db._name());
      const http = {
        GET: (route) => arango.GET(dbPrefix + route),
        POST: (route, body) => arango.POST(dbPrefix + route, body),
        DELETE: (route) => arango.DELETE(dbPrefix + route),
      };

      // The previous tests will have leftover entries in the
      // ReplicationClientsProgressTracker. So first, we look these up to not
      // choose a duplicate id, and be able to ignore them later.

      const existingClientSyncerIds = (() => {
        const {state: {running}, clients} = http.GET(`/_api/replication/logger-state`);
        assertTrue(running);
        assertInstanceOf(Array, clients);

        return new Set(clients.map(client => client.syncerId));
      })();
      const maxExistingSyncerId = Math.max(0, ...existingClientSyncerIds);

      const [syncer0, syncer1, syncer2] = _.range(maxExistingSyncerId + 1, maxExistingSyncerId + 4);

      // Get a snapshot
      const {lastTick: snapshotTick, id: replicationContextId}
        = http.POST(`/_api/replication/batch?syncerId=${syncer0}`, {ttl: 120});

      const callWailTail = (tick, syncerId) => {
        const result = http.GET(`/_api/wal/tail?from=${tick}&syncerId=${syncerId}`);
        assertFalse(result.error, `Expected call to succeed, but got ${JSON.stringify(result)}`);
        assertEqual(204, result.code, `Unexpected response ${JSON.stringify(result)}`);
      };

      callWailTail(snapshotTick, syncer1);
      callWailTail(snapshotTick, syncer2);

      // Now that the WAL should be held, release the snapshot.
      http.DELETE(`/_api/replication/batch/${replicationContextId}`);

      const getClients = () => {
        // e.g.
        // { "state": {"running": true, "lastLogTick": "71", "lastUncommittedLogTick": "71", "totalEvents": 71, "time": "2019-07-02T14:33:32Z"},
        //   "server": {"version": "3.5.0-devel", "serverId": "172021658338700", "engine": "rocksdb"},
        //   "clients": [
        //     {"syncerId": "102", "serverId": "", "time": "2019-07-02T14:33:32Z", "expires": "2019-07-02T16:33:32Z", "lastServedTick": "71"},
        //     {"syncerId": "101", "serverId": "", "time": "2019-07-02T14:33:32Z", "expires": "2019-07-02T16:33:32Z", "lastServedTick": "71"}
        //   ]}
        let {state: {running}, clients} = http.GET(`/_api/replication/logger-state`);
        assertTrue(running);
        assertInstanceOf(Array, clients);
        // remove clients that existed at the start of the test
        clients = clients.filter(client => !existingClientSyncerIds.has(client.syncerId));
        // sort ascending by syncerId
        clients.sort((a, b) => a.syncerId - b.syncerId);

        return clients;
      };

      let clients = getClients();
      assertEqual([syncer0, syncer1, syncer2], clients.map(client => client.syncerId));
      assertEqual(snapshotTick, clients[0].lastServedTick);
      assertEqual(snapshotTick, clients[1].lastServedTick);
      assertEqual(snapshotTick, clients[2].lastServedTick);

      // Update ticks
      callWailTail(parseInt(snapshotTick) + 1, syncer1);
      callWailTail(parseInt(snapshotTick) + 2, syncer2);

      clients = getClients();
      assertEqual([syncer0, syncer1, syncer2], clients.map(client => client.syncerId));
      assertEqual(snapshotTick, clients[0].lastServedTick);
      assertEqual((parseInt(snapshotTick) + 1).toString(), clients[1].lastServedTick);
      assertEqual((parseInt(snapshotTick) + 2).toString(), clients[2].lastServedTick);
    },
  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite on _system
// //////////////////////////////////////////////////////////////////////////////

function ReplicationSuite() {
  'use strict';

  let suite = {

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief set up
    // /////////////////////////////////////////////////////////////////////////////

    setUp: function() {
      connectToFollower();
      try {
        replication.applier.stop();
        replication.applier.forget();
      } catch (err) {
      }
      connectToLeader();

      db._drop(cn);
      db._drop(cn2);
      db._drop(systemCn, {isSystem: true});
    },

    // /////////////////////////////////////////////////////////////////////////////
    //  @brief tear down
    // /////////////////////////////////////////////////////////////////////////////

    tearDown: function() {
      connectToLeader();

      db._drop(cn);
      db._drop(cn2);
      db._drop(systemCn, {isSystem: true});
      db._dropView("UnitTestsSyncSearchAlias");
      db._dropView("UnitTestsSyncView");

      connectToFollower();
      replication.applier.stop();
      replication.applier.forget();
      db._drop(cn);
      db._drop(cn2);
      db._drop(systemCn, {isSystem: true});
      db._dropView("UnitTestsSyncSearchAlias");
      db._dropView("UnitTestsSyncView");
    }
  };
  deriveTestSuite(BaseTestConfig(), suite, '_Repl');

  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite on other DB
// //////////////////////////////////////////////////////////////////////////////

function ReplicationOtherDBSuiteBase(testDB) {
  let suite = {

    setUp: function() {
      db._useDatabase('_system');
      // Follower side code
      connectToFollower();
      try {
        replication.applier.stop();
        replication.applier.forget();
      } catch (err) {
      }
      try {
        db._dropDatabase(testDB);
      } catch (e) {
      }
      db._createDatabase(testDB);
      assertNotEqual(-1, db._databases().indexOf(testDB));

      // Leader side code
      connectToLeader();

      try {
        db._dropDatabase(testDB);
      } catch (e) {
      }
      db._createDatabase(testDB);

      assertNotEqual(-1, db._databases().indexOf(testDB));
      // We now have an empty testDB
      db._useDatabase(testDB);
      // Use it and setup replication
    },

    tearDown: function() {
      // Just drop the databases
      connectToLeader();
      db._useDatabase('_system');
      try {
        db._dropDatabase(testDB);
      } catch (e) {
      }

      assertEqual(-1, db._databases().indexOf(testDB));

      connectToFollower();
      // We need to stop applier in testDB
      db._useDatabase(testDB);
      replication.applier.stop();
      replication.applier.forget();
      db._useDatabase('_system');
      try {
        db._dropDatabase(testDB);
      } catch (e) {
      }

      assertEqual(-1, db._databases().indexOf(testDB));
    }
  };

  deriveTestSuite(BaseTestConfig(), suite, '_' + testDB);

  return suite;
}

function ReplicationOtherDBTraditionalNameSuite() {
  return ReplicationOtherDBSuiteBase('UnitTestDB');
}

function ReplicationOtherDBExtendedNameSuite() {
  return ReplicationOtherDBSuiteBase("Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨");
}



jsunity.run(ReplicationSuite);
jsunity.run(ReplicationOtherDBTraditionalNameSuite);
jsunity.run(ReplicationOtherDBExtendedNameSuite);



return jsunity.done();
