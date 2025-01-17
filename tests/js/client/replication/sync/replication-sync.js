/* jshint globalstrict:false, strict:false, unused: false */
/* global arango, assertEqual, assertNotEqual, assertTrue, assertFalse, assertNotNull, ARGUMENTS */

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
const { deriveTestSuite, getMetric, versionHas } = require('@arangodb/test-helper');
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
const db = arangodb.db;
const _ = require('lodash');

const replication = require('@arangodb/replication');
const internal = require('internal');

const cn = 'UnitTestsReplication';
const sysCn = '_UnitTestsReplication';
const userManager = require("@arangodb/users");
let IM = global.instanceManager;
const leaderEndpoint = IM.arangods[0].endpoint;
const followerEndpoint = IM.arangods[1].endpoint;

const connectToLeader = function() {
  reconnectRetry(leaderEndpoint, db._name(), 'root', '');
  db._flushCache();
};

const connectToFollower = function() {
  reconnectRetry(followerEndpoint, db._name(), 'root', '');
  db._flushCache();
};

const collectionChecksum = function (name) {
  return db._collection(name).checksum(true, true).checksum;
};

const collectionCount = function (name) {
  return db._collection(name).count();
};

const compare = function (leaderFunc, followerInitFunc, followerCompareFunc, incremental, system) {
  let state = {};
  
  db._flushCache();
  leaderFunc(state);

  connectToFollower();

  followerInitFunc(state);

  replication.syncCollection(system ? sysCn : cn, {
    endpoint: leaderEndpoint,
    verbose: true,
    includeSystem: true,
    incremental,
    username: "root",
    password: "",
  });

  db._flushCache();
  followerCompareFunc(state);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief Base Test Config. Identitical part for _system and other DB
// //////////////////////////////////////////////////////////////////////////////

function BaseTestConfig () {
  'use strict';

  return {
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////
    
    testExistingPatchBrokenFollowerCounters1: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      compare(
        function (state) {
          try {
            arango.PUT_RAW("/_admin/debug/failat/disableRevisionsAsDocumentIds", "");

            let c = db._create(cn);
            let docs = [];

            for (let i = 0; i < 5000; ++i) {
              docs.push({ value: i });
            }
            c.insert(docs);

            state.checksum = collectionChecksum(cn);
            state.count = collectionCount(cn);
            assertEqual(5000, state.count);
          } finally {
            arango.DELETE_RAW("/_admin/debug/failat", "");
          }
        },
        function (state) {
          arango.PUT_RAW("/_admin/debug/failat/disableRevisionsAsDocumentIds", "");

          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",

          });
          // collection present on follower now
          var c = db._collection(cn);
          assertEqual(5000, c.count());
          assertEqual(5000, c.toArray().length);
          try {
            arango.PUT_RAW("/_admin/debug/failat/RocksDBCommitCounts", "");
            c.insert({});
          } finally {
            arango.DELETE_RAW("/_admin/debug/failat", "");
          }
          assertEqual(5000, c.count());
          assertEqual(5001, c.toArray().length);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
          // now validate counters
          let c = db._collection(cn);
          assertEqual(5000, c.toArray().length);
          assertEqual(5000, c.count());
        },
        true
      );
    },
    
    testExistingPatchBrokenFollowerCounters2: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      compare(
        function (state) {
          try {
            arango.PUT_RAW("/_admin/debug/failat/disableRevisionsAsDocumentIds", "");
            let c = db._create(cn);
            let docs = [];

            for (let i = 0; i < 10000; ++i) {
              docs.push({ value: i, _key: "test" + i });
            }
            c.insert(docs);

            state.checksum = collectionChecksum(cn);
            state.count = collectionCount(cn);
            assertEqual(10000, state.count);
          } finally {
            arango.DELETE_RAW("/_admin/debug/failat", "");
          }
        },
        function (state) {
          try {
            arango.PUT_RAW("/_admin/debug/failat/disableRevisionsAsDocumentIds", "");

            //  already create the collection on the follower
            replication.syncCollection(cn, {
              endpoint: leaderEndpoint,
              incremental: false,
              username: "root",
              password: "",
            });

            // collection present on follower now
            var c = db._collection(cn);
            for (let i = 0; i < 10000; i += 10) {
              c.remove("test" + i);
            }
            assertEqual(9000, c.count());
            assertEqual(9000, c.toArray().length);
            arango.PUT_RAW("/_admin/debug/failat/RocksDBCommitCounts", "");
            for (let i = 0; i < 100; ++i) {
              c.insert({ _key: "testmann" + i });
            }
          } finally {
            arango.DELETE_RAW("/_admin/debug/failat", "");
          }
          assertEqual(9000, c.count());
          assertEqual(9100, c.toArray().length);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
          // now validate counters
          let c = db._collection(cn);
          assertEqual(10000, c.count());
          assertEqual(10000, c.toArray().length);
        },
        true
      );
    },

    testExistingIndexId: function () {
      connectToLeader();

      compare(
        function (state) {
          const c = db._create(cn);
          state.indexDef = {type: 'skiplist', id: '1234567', fields: ['value']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.id);
        },
        function (state) {
          //  already create the collection and index on the follower
          const c = db._create(cn);
          c.ensureIndex(state.indexDef);
        },
        function (state) {
          const c = db._collection(cn);
          const i = c.index(state.indexDef.id);
          assertEqual(state.leaderProps.id, i.id);
          assertEqual(state.leaderProps.name, i.name);
        },
        true
      );
    },

    testExistingIndexIdConflict: function () {
      connectToLeader();

      compare(
        function (state) {
          const c = db._create(cn);
          state.indexDef = {type: 'hash', id: '1234567', fields: ['value']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.id);
        },
        function (state) {
          //  already create the collection and index on the follower
          const c = db._create(cn);
          const def = {type: 'skiplist', id: '1234567', fields: ['value2']};
          c.ensureIndex(def);
        },
        function (state) {
          const c = db._collection(cn);
          const i = c.index(state.indexDef.id);
          assertEqual(state.indexDef.type, i.type);
          assertEqual(state.leaderProps.id, i.id);
          assertEqual(state.leaderProps.name, i.name);
        },
        true
      );
    },

    testExistingSystemIndexIdConflict: function () {
      connectToLeader();
      
      compare(
        function (state) {
          const c = db._create(sysCn, {isSystem: true});
          state.indexDef = {type: 'hash', id: '1234567', fields: ['value']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.id);
        },
        function (state) {
          //  already create the index on the follower
          const c = db._create(sysCn, {isSystem: true});
          const def = {type: 'skiplist', id: '1234567', fields: ['value2']};
          c.ensureIndex(def);
        },
        function (state) {
          const c = db._collection(sysCn);
          const i = c.index(state.indexDef.id);
          assertEqual(state.indexDef.type, i.type);
          assertEqual(state.leaderProps.id, i.id);
          assertEqual(state.leaderProps.name, i.name);
        },
        true, true
      );
    },

    testExistingIndexName: function () {
      connectToLeader();

      compare(
        function (state) {
          const c = db._create(cn);
          state.indexDef = {type: 'skiplist', name: 'foo', fields: ['value']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.name);
        },
        function (state) {
          //  already create the collection and index on the follower
          const c = db._create(cn);
          c.ensureIndex(state.indexDef);
        },
        function (state) {
          const c = db._collection(cn);
          const i = c.index(state.indexDef.name);
          assertEqual(state.leaderProps.id, i.id);
          assertEqual(state.leaderProps.name, i.name);
        },
        true
      );
    },

    testExistingIndexNameConflict: function () {
      connectToLeader();

      compare(
        function (state) {
          const c = db._create(cn);
          state.indexDef = {type: 'hash', name: 'foo', fields: ['value']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.name);
        },
        function (state) {
          //  already create the collection and index on the follower
          const c = db._create(cn);
          const def = {type: 'skiplist', name: 'foo', fields: ['value2']};
          c.ensureIndex(def);
        },
        function (state) {
          const c = db._collection(cn);
          const i = c.index(state.indexDef.name);
          assertEqual(state.indexDef.type, i.type);
        },
        true
      );
    },

    testExistingSystemIndexNameConflict: function () {
      connectToLeader();
      
      compare(
        function (state) {
          const c = db._create(sysCn, {isSystem: true});
          state.indexDef = {type: 'hash', name: 'foo', fields: ['value3']};
          c.ensureIndex(state.indexDef);
          state.leaderProps = c.index(state.indexDef.name);
        },
        function (state) {
          //  already create the index on the follower
          const c = db._create(sysCn, {isSystem: true});
          const def  = {type: 'skiplist', name: 'foo', fields: ['value4']};
          c.ensureIndex(def);
        },
        function (state) {
          const c = db._collection(sysCn);
          const i = c.index(state.indexDef.name);
          assertEqual(state.indexDef.type, i.type);
        },
        true, true
      );
    },
    
    testLeaderHasMoreData : function () {
      connectToLeader();

      let st = {};
      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value1: i, value2: 'test' + i });
          }

          while (c.count() < 100 * 1000) {
            c.insert(docs);
          }

          // sync the initial part to follower
          connectToFollower();
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            verbose: true,
            includeSystem: true,
            incremental: true,
            username: "root",
            password: "",
          });

          // remove docs from leader
          connectToLeader();
          while (db[cn].count() <= 240000) {
            db._query(`FOR i IN 1..10000 INSERT { value1: i, value2: CONCAT('test', i) } IN ${cn}`);
          }
          st = _.clone({ count: collectionCount(cn), checksum: collectionChecksum(cn) });
        },
        function (state) {
        },
        function (state) {
          assertEqual(st.count, collectionCount(cn));
          assertEqual(st.checksum, collectionChecksum(cn));
        },
        true
      );
    },
    
    testFollowerHasMoreData : function () {
      connectToLeader();

      let st = {};
      const count = versionHas('tsan') ? 100 : 1000;
      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value1: i, value2: 'test' + i });
          }

          while (c.count() < 500 * count) {
            c.insert(docs);
          }

          // sync the initial part to follower
          connectToFollower();
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            verbose: true,
            includeSystem: true,
            incremental: true,
            username: "root",
            password: "",
          });

          // remove docs from leader
          connectToLeader();
          while (db[cn].count() > 50 * count) {
            db._query(`FOR doc IN ${cn} LIMIT 20*@count REMOVE doc IN ${cn}`, {count});
          }
          st = _.clone({ count: collectionCount(cn), checksum: collectionChecksum(cn) });
        },
        function (state) {
        },
        function (state) {
          assertEqual(st.count, collectionCount(cn));
          assertEqual(st.checksum, collectionChecksum(cn));
        },
        true
      );
    },
    
    testLeaderHasDifferentData : function () {
      connectToLeader();

      let st = {};
      compare(
        function (state) {
          let c = db._create(cn);
      
          // create empty collection on follower already
          connectToFollower();
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            verbose: true,
            incremental: true,
            username: "root",
            password: "",
          });

          connectToLeader();
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value1: i, value2: 'test' + i });
          }

          while (c.count() < 200 * 1000) {
            c.insert(docs);
          }
          st = _.clone({ count: collectionCount(cn), checksum: collectionChecksum(cn) });
        },
        function (state) {
          while (db[cn].count() <= 150000) {
            db._query(`FOR i IN 1..10000 INSERT { value1: i, value2: CONCAT('test', i) } IN ${cn}`);
          }
        },
        function (state) {
          assertEqual(st.count, collectionCount(cn));
          assertEqual(st.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testRemoveSomeWithIncremental: function () {
      connectToLeader();

      var st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ _key: 'test' + i });
          }

          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToFollower();

      assertEqual(5000, collectionCount(cn));
      var c = db._collection(cn);
      //  remove some random documents
      for (var i = 0; i < 50; ++i) {
        c.remove(c.any());
      }
      assertEqual(4950, collectionCount(cn));

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));
    },
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testInsertSomeWithIncremental: function () {
      connectToLeader();

      var st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ _key: 'test' + i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToFollower();

      let c = db._collection(cn);
      //  insert some random documents
      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ foo: 'bar' + i });
      }
      c.insert(docs);

      assertEqual(5100, collectionCount(cn));

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testInsertHugeIncrementalMoreOnLeader: function () {
      connectToLeader();

      var st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ _key: 'test' + i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToLeader();
      var c = db._collection(cn);
      let docs = [];
      //  insert some documents 'before'
      for (let i = 0; i < 110000; ++i) {
        docs.push({ _key: 'a' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }

      //  insert some documents 'after'
      for (let i = 0; i < 110000; ++i) {
        docs.push({ _key: 'z' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }

      //  update the state
      st.checksum = collectionChecksum(cn);
      st.count = collectionCount(cn);
      assertEqual(225000, collectionCount(cn));

      connectToFollower();

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));
    },

    testInsertHugeIncrementalMoreOnFollower: function () {
      connectToLeader();

      var st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ _key: 'test' + i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToFollower();

      var c = db._collection(cn);
      let docs = [];
      //  insert some documents 'before'
      for (let i = 0; i < 110000; ++i) {
        docs.push({ _key: 'a' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }

      //  insert some documents 'after'
      for (let i = 0; i < 110000; ++i) {
        docs.push({ _key: 'z' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }

      assertEqual(225000, collectionCount(cn));

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testInsertHugeIncrementalLessOnLeader: function () {
      connectToLeader();

      var st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];
          //  insert some documents 'before'
          for (let i = 0; i < 110000; ++i) {
            docs.push({ _key: 'a' + i });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          //  insert some documents 'after'
          for (let i = 0; i < 110000; ++i) {
            docs.push({ _key: 'z' + i });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(220000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToLeader();
      var c = db._collection(cn);
      let docs = [];
      //  remove some documents from the 'front'
      for (let i = 0; i < 50000; ++i) {
        docs.push({ _key: 'a' + i });
        if (docs.length === 5000) {
          c.remove(docs);
          docs = [];
        }
      }

      //  remove some documents from the 'back'
      for (let i = 0; i < 50000; ++i) {
        docs.push({ _key: 'z' + i });
        if (docs.length === 5000) {
          c.remove(docs);
          docs = [];
        }
      }

      //  update the state
      st.checksum = collectionChecksum(cn);
      st.count = collectionCount(cn);
      assertEqual(120000, collectionCount(cn));

      connectToFollower();

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));
    },

    testUpdateHugeIntermediateCommits: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      let st;

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];
          //  insert some documents 'before'
          for (let i = 0; i < 55000; ++i) {
            docs.push({ _key: 'a' + i });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          //  insert some documents 'after'
          for (let i = 0; i < 55000; ++i) {
            docs.push({ _key: 'z' + i });
            if (docs.length === 5000) {
              c.insert(docs);
              docs = [];
            }
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(110000, state.count);

          st = _.clone(state); // save state
        },
        function (state) {
          arango.PUT_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );

      connectToLeader();
      let c = db._collection(cn);
      let selectors = [];
      let docs = [];
      //  update some documents at the 'front'
      for (let i = 0; i < 25000; ++i) {
        selectors.push({ _key: 'a' + i });
        docs.push({ updated: true });
        if (docs.length === 5000) {
          c.update(selectors, docs);
          selectors = [];
          docs = [];
        }
      }

      //  remove some documents from the 'back'
      for (let i = 0; i < 25000; ++i) {
        selectors.push({ _key: 'z' + i });
        docs.push({ updated: true });
        if (docs.length === 5000) {
          c.update(selectors, docs);
          selectors = [];
          docs = [];
        }
      }

      //  update the state
      st.checksum = collectionChecksum(cn);
      st.count = collectionCount(cn);
      assertEqual(110000, collectionCount(cn));

      connectToFollower();

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      assertEqual(st.count, collectionCount(cn));
      assertEqual(st.checksum, collectionChecksum(cn));

      arango.DELETE_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");
    },
    
    testIntermediateCommitsUsageOnInsert: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ _key: "test" + i });
      }
      c.insert(docs);
      
      connectToFollower();
      
      // sync almost empty collection to follower
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });
     
      // add a lot more data on the leader
      connectToLeader();
      c = db._collection(cn);
      docs = [];
      for (let i = 0; i < 50000; ++i) {
        docs.push({ _key: 'testmann' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }
          
      assertEqual(50100, collectionCount(cn));
      let checksum = collectionChecksum(cn);

      connectToFollower();
          
      // set intermediate commit count to 1000 on follower
      arango.PUT_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");

      let intermediateCommitsBefore = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      assertEqual(50100, collectionCount(cn));
      assertEqual(checksum, collectionChecksum(cn));
      
      arango.DELETE_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");
      
      let intermediateCommitsAfter = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");
      // unfortunately we don't know the exact number of intermediate commits that have
      // been made. this is because the intermediate commits will not be triggered
      // precisely at the specified intermediate commit count value of 1000, but only
      // after each individual batch, which can contain arbitrary amounts of documents.
      assertTrue(intermediateCommitsAfter > intermediateCommitsBefore, { intermediateCommitsBefore, intermediateCommitsAfter });
    },
    
    testIntermediateCommitsUsageOnUpdate: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < 50000; ++i) {
        docs.push({ _key: 'testmann' + i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }
      
      connectToFollower();
      
      // sync collection to follower
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });
     
      // update documents on the leader
      connectToLeader();
      db._query("FOR doc IN " + cn + " UPDATE doc WITH { value: 42 } IN " + cn);
          
      assertEqual(50000, collectionCount(cn));
      let checksum = collectionChecksum(cn);

      connectToFollower();
          
      // set intermediate commit count to 1000 on follower
      arango.PUT_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");

      let intermediateCommitsBefore = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");

      //  and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      assertEqual(50000, collectionCount(cn));
      assertEqual(checksum, collectionChecksum(cn));
      
      arango.DELETE_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");
      
      let intermediateCommitsAfter = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");
      // unfortunately we don't know the exact number of intermediate commits that have
      // been made. this is because the intermediate commits will not be triggered
      // precisely at the specified intermediate commit count value of 1000, but only
      // after each individual batch, which can contain arbitrary amounts of documents.
      assertTrue(intermediateCommitsAfter > intermediateCommitsBefore, { intermediateCommitsBefore, intermediateCommitsAfter });
    },
    
    testIntermediateCommitsUsageOnRemove: function () {
      // can only use this with failure tests enabled
      let r = arango.GET("/_admin/debug/failat");
      if (String(r) === "false" || r.error) {
        return;
      }

      connectToLeader();

      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < 50000; ++i) {
        docs.push({ _key: 'testmann' + i, value: i });
        if (docs.length === 5000) {
          c.insert(docs);
          docs = [];
        }
      }
      
      connectToFollower();
      
      // sync collection to follower
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });
     
      // remove half the documents on the leader
      connectToLeader();
      db._query("FOR doc IN " + cn + " FILTER doc.value >= 25000 REMOVE doc IN " + cn);
          
      assertEqual(25000, collectionCount(cn));
      let checksum = collectionChecksum(cn);

      connectToFollower();
          
      // set intermediate commit count to 1000 on follower
      arango.PUT_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");

      let intermediateCommitsBefore = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");

      // and sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      assertEqual(25000, collectionCount(cn));
      assertEqual(checksum, collectionChecksum(cn));
      
      arango.DELETE_RAW("/_admin/debug/failat/IncrementalReplicationFrequentIntermediateCommit", "");
      
      let intermediateCommitsAfter = getMetric(arango.getEndpoint(), "arangodb_intermediate_commits_total");
      // unfortunately we don't know the exact number of intermediate commits that have
      // been made. this is because the intermediate commits will not be triggered
      // precisely at the specified intermediate commit count value of 1000, but only
      // after each individual batch, which can contain arbitrary amounts of documents.
      assertTrue(intermediateCommitsAfter > intermediateCommitsBefore, { intermediateCommitsBefore, intermediateCommitsAfter });
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test collection properties
    // //////////////////////////////////////////////////////////////////////////////

    testProperties: function () {
      connectToLeader();

      compare(
        function (state) {
          var c = db._create(cn);

          c.properties({
            waitForSync: true,
          });
        },
        function (state) {
          //  don't create the collection on the follower
        },
        function (state) {
          var c = db._collection(cn);
          var p = c.properties();
          assertTrue(p.waitForSync);
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test collection properties
    // //////////////////////////////////////////////////////////////////////////////

    testPropertiesOther: function () {
      connectToLeader();

      compare(
        function (state) {
          var c = db._create(cn);

          c.properties({
            waitForSync: true,
          });
        },
        function (state) {
          //  create the collection on the follower, but with default properties
          db._create(cn);
        },
        function (state) {
          var c = db._collection(cn);
          var p = c.properties();
          assertTrue(p.waitForSync);
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with indexes
    // //////////////////////////////////////////////////////////////////////////////

    testCreateIndexes: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              'value1': i,
              'value2': 'test' + i
            });
          }
          c.insert(docs);

          c.ensureIndex({
            type: 'hash',
            fields: ['value1', 'value2']
          });
          c.ensureIndex({
            type: 'skiplist',
            fields: ['value1']
          });

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];

          //  create it with different values
          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              'value1': i,
              'value2': 'test' + i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var idx = db._collection(cn).indexes();
          assertEqual(3, idx.length); // primary + hash + skiplist
          for (var i = 1; i < idx.length; ++i) {
            assertFalse(idx[i].unique);
            assertFalse(idx[i].sparse);

            if (idx[i].type === 'hash') {
              assertEqual('hash', idx[i].type);
              assertEqual(['value1', 'value2'], idx[i].fields);
            } else {
              assertEqual('skiplist', idx[i].type);
              assertEqual(['value1'], idx[i].fields);
            }
          }
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with views
    // //////////////////////////////////////////////////////////////////////////////

    testSyncView: function () {
      connectToLeader();

      // create custom analyzer
      let analyzer = analyzers.save('custom', 'delimiter', { delimiter: ' ' }, ['frequency']);

      //  create view & collection on leader
      db._flushCache();
      db._create(cn);
      db._createView(cn + 'View', 'arangosearch', {consolidationIntervalMsec:0});

      connectToFollower();
      //  sync on follower
      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      db._flushCache();
      {
        //  check state is the same
        let view = db._view(cn + 'View');
        assertNotNull(view);
        assertEqual(cn + "View", view.name());
        assertEqual("arangosearch", view.type());
        let props = view.properties();
        assertTrue(props.hasOwnProperty('links'));
        assertEqual(0, props.consolidationIntervalMsec);
        assertEqual(Object.keys(props.links).length, 0);
      }

      connectToLeader();

      //  update view properties
      {
        let view = db._view(cn + 'View');
        let links = {};
        links[cn] = {
          includeAllFields: true,
          fields: {
              text: { analyzers: ['text_en', 'custom' ] }
          }
        };
        view.properties({ links });
      }

      connectToFollower();

      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      // check replicated analyzer
      {
        let analyzerFollower = analyzers.analyzer('custom');
        assertEqual(analyzer.name, analyzerFollower.name());
        assertEqual(analyzer.type, analyzerFollower.type());
        assertEqual(analyzer.properties, analyzerFollower.properties());
        assertEqual(analyzer.features, analyzerFollower.features());
      }

      {
        let view = db._view(cn + 'View');
        assertNotEqual(view, null);
        assertEqual(cn + "View", view.name());
        assertEqual("arangosearch", view.type());
        let props = view.properties();
        assertEqual(0, props.consolidationIntervalMsec);
        assertTrue(props.hasOwnProperty('links'));
        assertEqual(Object.keys(props.links).length, 1);
        assertTrue(props.links.hasOwnProperty(cn));
        assertTrue(props.links[cn].includeAllFields);
        assertEqual(Object.keys(props.links[cn].fields).length, 1);
        assertEqual(props.links[cn].fields.text.analyzers.length, 2);
        assertEqual(props.links[cn].fields.text.analyzers[0], 'text_en');
        assertEqual(props.links[cn].fields.text.analyzers[1], 'custom');
      }
    },

    testSyncViewOnAnalyzersCollection: function () {
      connectToLeader();

      // create custom analyzer
      assertNotEqual(null, db._collection("_analyzers"));
      assertEqual(0, db._analyzers.count());
      let analyzer = analyzers.save('custom', 'delimiter', { delimiter: ' ' }, ['frequency']);
      assertEqual(1, db._analyzers.count());
      // check analyzer is usable on leader
      assertEqual(3, db._query("RETURN TOKENS('1 2 3', 'custom')").toArray()[0].length);

      //  create view & collection on leader
      db._flushCache();
      db._createView('analyzersView', 'arangosearch', {
        consolidationIntervalMsec:0,
        links: { _analyzers: { analyzers: [ analyzer.name ], includeAllFields:true } }
      });

      connectToFollower();
      //  sync on follower
      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      db._flushCache();

      assertEqual(1, db._analyzers.count());
      var res = db._query("FOR d IN analyzersView OPTIONS {waitForSync:true} RETURN d").toArray();
      assertEqual(1, db._analyzers.count());
      assertEqual(1, res.length);
      assertEqual(db._analyzers.toArray()[0], res[0]);
      
      // check analyzer is usable on follower
      assertEqual(3, db._query("RETURN TOKENS('1 2 3', 'custom')").toArray()[0].length);

      {
        //  check state is the same
        let view = db._view('analyzersView');
        assertNotNull(view);
        let props = view.properties();
        assertTrue(props.hasOwnProperty('links'));
        assertEqual(0, props.consolidationIntervalMsec);
        assertEqual(Object.keys(props.links).length, 1);
        assertTrue(props.links.hasOwnProperty('_analyzers'));
        assertTrue(props.links['_analyzers'].includeAllFields);
        assertTrue(props.links['_analyzers'].analyzers.length, 1);
        assertTrue(props.links['_analyzers'].analyzers[0], 'custom');
      }
    },
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with search-alias views
    // //////////////////////////////////////////////////////////////////////////////

    testSyncSearchAliasViewWithIndexes: function () {
      connectToLeader();

      //  create view & collection on leader
      let c = db._create(cn);
      let idx = c.ensureIndex({ type: "inverted", fields: [ { name: "value" } ], name: "inverted_idx" });
      let view = db._createView(cn + 'View', 'search-alias', {
        indexes: [
          { 
            collection: cn,
            index: idx.name
          }
        ]
      });
      assertEqual(1, view.properties().indexes.length);

      connectToFollower();
      //  sync on follower
      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      db._flushCache();
      {
        //  check state is the same
        let view = db._view(cn + 'View');
        assertNotNull(view);
        assertEqual(cn + "View", view.name());
        assertEqual("search-alias", view.type());
        let props = view.properties();
        assertTrue(props.hasOwnProperty('indexes'));
        assertEqual(1, props.indexes.length);
        assertEqual({ collection: cn, index: idx.name }, props.indexes[0]);
      }
    },
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with search-alias views
    // //////////////////////////////////////////////////////////////////////////////

    testSyncSearchAliasViewChange: function () {
      connectToLeader();

      //  create view & collection on leader
      let c = db._create(cn);
      let idx = c.ensureIndex({ type: "inverted", fields: [ { name: "value" } ], name: "inverted_idx" });
      let view = db._createView(cn + 'View', 'search-alias', {});
      assertEqual(0, view.properties().indexes.length);

      connectToFollower();
      //  sync on follower
      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      db._flushCache();
      {
        //  check state is the same
        let view = db._view(cn + 'View');
        assertNotNull(view);
        assertEqual(cn + "View", view.name());
        assertEqual("search-alias", view.type());
        let props = view.properties();
        assertTrue(props.hasOwnProperty('indexes'));
        assertEqual(0, props.indexes.length);
      }

      connectToLeader();

      //  update view properties
      {
        let view = db._view(cn + 'View');
        view.properties({ 
          indexes: [
            { 
              collection: cn,
              index: idx.name
            }
          ]
        });
        assertEqual(1, view.properties().indexes.length);
      }

      connectToFollower();

      replication.sync({ endpoint: leaderEndpoint, username: "root", password: "" });

      {
        let view = db._view(cn + 'View');
        assertNotEqual(view, null);
        assertEqual(cn + "View", view.name());
        assertEqual("search-alias", view.type());
        let props = view.properties();
        assertTrue(props.hasOwnProperty('indexes'));
        assertEqual(1, props.indexes.length, props.indexes);
        assertEqual({ collection: cn, index: idx.name }, props.indexes[0]);
      }
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with edges
    // //////////////////////////////////////////////////////////////////////////////

    testEdges: function () {
      connectToLeader();

      compare(
        function (state) {
          var c = db._createEdgeCollection(cn);
          var i;

          for (i = 0; i < 100; ++i) {
            c.save(cn + '/test' + i, cn + '/test' + (i % 10), {
              _key: 'test' + i,
              'value1': i,
              'value2': 'test' + i
            });
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(100, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];
          for (var i = 0; i < 100; ++i) {
            docs.push(cn + '/test' + i, cn + '/test' + (i % 10), {
              _key: 'test' + i,
              'value1': i,
              'value2': 'test' + i
            });
          }
          c.save(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var c = db._collection(cn);
          assertEqual(3, c.type());

          for (var i = 0; i < 100; ++i) {
            var doc = c.document('test' + i);
            assertEqual(cn + '/test' + i, doc._from);
            assertEqual(cn + '/test' + (i % 10), doc._to);
            assertEqual(i, doc.value1);
            assertEqual('test' + i, doc.value2);
          }
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with edges differences
    // //////////////////////////////////////////////////////////////////////////////

    testEdgesDifferences: function () {
      connectToLeader();

      compare(
        function (state) {
          var c = db._createEdgeCollection(cn);
          var i;

          for (i = 0; i < 100; ++i) {
            c.save(cn + '/test' + i, cn + '/test' + (i % 10), {
              _key: 'test' + i,
              'value1': i,
              'value2': 'test' + i
            });
          }

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(100, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate(); // but empty it

          let docs = [];
          for (var i = 0; i < 200; ++i) {
            docs.push(
              cn + '/test' + (i + 1),
              cn + '/test' + (i % 11), {
                _key: 'test' + i,
                'value1': i,
                'value2': 'test' + i
              }
            );
          }
          c.save(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));

          var c = db._collection(cn);
          assertEqual(3, c.type());

          for (var i = 0; i < 100; ++i) {
            var doc = c.document('test' + i);
            assertEqual(cn + '/test' + i, doc._from);
            assertEqual(cn + '/test' + (i % 10), doc._to);
            assertEqual(i, doc.value1);
            assertEqual('test' + i, doc.value2);
          }
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test non-present collection
    // //////////////////////////////////////////////////////////////////////////////

    testNonPresent: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) { },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test non-present collection
    // //////////////////////////////////////////////////////////////////////////////

    testNonPresentIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) { },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testExisting: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate(); // but empty it
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test existing collection
    // //////////////////////////////////////////////////////////////////////////////

    testExistingIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate(); // but empty it
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - but empty on leader
    // //////////////////////////////////////////////////////////////////////////////

    testExistingEmptyOnLeader: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(0, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate(); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - but empty on leader
    // //////////////////////////////////////////////////////////////////////////////

    testExistingEmptyOnLeaderIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          db._create(cn);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(0, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate(); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - less on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testExistingDocumentsLess: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate(); // but empty it

          for (var i = 0; i < 500; ++i) {
            c.save({
              'value': i
            });
          }
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - less on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testExistingDocumentsLessIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          var c = db._collection(cn);
          c.truncate(); // but empty it

          for (var i = 0; i < 500; ++i) {
            c.save({
              'value': i
            });
          }
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - more on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testMoreDocuments: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate(); // but empty it

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - more on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testMoreDocumentsIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate(); // but empty it

          let docs = [];
          for (let i = 0; i < 6000; ++i) {
            docs.push({ value: i });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testSameDocuments: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value: i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower
    // //////////////////////////////////////////////////////////////////////////////

    testSameDocumentsIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value: i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },
    
    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower but different keys
    // //////////////////////////////////////////////////////////////////////////////

    testSameSameButDifferentKeys: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              value: i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              value: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower but different keys
    // //////////////////////////////////////////////////////////////////////////////

    testSameSameButDifferentKeysIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              value: i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              value: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower but different values
    // //////////////////////////////////////////////////////////////////////////////

    testSameSameButDifferentValues: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value1: i,
              value2: 'test' + i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value1: 'test' + i,
              value2: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        false
      );
    },

    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test with existing documents - same on the follower but different values
    // //////////////////////////////////////////////////////////////////////////////

    testSameSameButDifferentValuesIncremental: function () {
      connectToLeader();

      compare(
        function (state) {
          let c = db._create(cn);
          let docs = [];

          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value1: i,
              value2: 'test' + i
            });
          }
          c.insert(docs);

          state.checksum = collectionChecksum(cn);
          state.count = collectionCount(cn);
          assertEqual(5000, state.count);
        },
        function (state) {
          //  already create the collection on the follower
          replication.syncCollection(cn, {
            endpoint: leaderEndpoint,
            incremental: false,
            username: "root",
            password: "",
          });
          let c = db._collection(cn);
          c.truncate({ compact: false }); // but empty it

          let docs = [];
          for (let i = 0; i < 5000; ++i) {
            docs.push({
              _key: 'test' + i,
              value1: 'test' + i,
              value2: i
            });
          }
          c.insert(docs);
        },
        function (state) {
          assertEqual(state.count, collectionCount(cn));
          assertEqual(state.checksum, collectionChecksum(cn));
        },
        true
      );
    }
  
  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite on _system
// //////////////////////////////////////////////////////////////////////////////

function ReplicationSuite () {
  'use strict';

  let suite = {
    setUp: function () {
      connectToLeader();
      arango.DELETE_RAW("/_admin/debug/failat", "");
      try {
        db._dropView(cn + 'View');
      } catch (ignored) {}
      try {
        db._dropView('analyzersView');
      } catch (ignored) {}
      db._drop(cn);
      db._drop(sysCn, {isSystem: true});
    },

    tearDown: function () {
      connectToLeader();
      arango.DELETE_RAW("/_admin/debug/failat", "");
      try {
        db._dropView(cn + 'View');
      } catch (ignored) {}
      try {
        db._dropView('analyzersView');
      } catch (ignored) {}
      db._drop(cn);
      db._drop(sysCn, {isSystem: true});
      try {
        analyzers.remove('custom');
      } catch (e) { }
      try {
        analyzers.remove('smartCustom');
      } catch (e) { }
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });

      connectToFollower();
      arango.DELETE_RAW("/_admin/debug/failat", "");
      try {
        db._dropView(cn + 'View');
      } catch (ignored) {}
      try {
        db._dropView('analyzersView');
      } catch (ignored) {}
      db._drop(cn);
      db._drop(sysCn, {isSystem: true});
      try {
        analyzers.remove('custom');
      } catch (e) { }
      try {
        analyzers.remove('smartCustom');
      } catch (e) { }
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });
    }
  };
  
  deriveTestSuite(BaseTestConfig(), suite, '_Repl');
  return suite;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite on other database
// //////////////////////////////////////////////////////////////////////////////

function ReplicationOtherDBSuiteBase (dbName) {
  'use strict';

  let suite = {

    setUp: function () {
      connectToLeader();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
      db._createDatabase(dbName);
      connectToFollower();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
      db._createDatabase(dbName);
      db._useDatabase(dbName);
      connectToLeader();
    },

    tearDown: function () {
      db._useDatabase('_system');
      connectToLeader();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });

      connectToFollower();
      try {
        db._dropDatabase(dbName);
      } catch (e) {
      }
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });
    }
  };

  deriveTestSuite(BaseTestConfig(), suite, '_' + dbName);
  return suite;
}

function ReplicationOtherDBTraditionalNameSuite () {
  return ReplicationOtherDBSuiteBase('UnitTestDB');
}

function ReplicationOtherDBExtendedNameSuite () {
  return ReplicationOtherDBSuiteBase("Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨");
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite for key conflicts in incremental sync
// //////////////////////////////////////////////////////////////////////////////

function ReplicationIncrementalKeyConflict () {
  'use strict';

  return {

    setUp: function () {
      connectToLeader();
      db._drop(cn);
    },

    tearDown: function () {
      connectToLeader();
      db._drop(cn);
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });

      connectToFollower();
      db._drop(cn);
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });
    },

    testKeyConflictsIncremental: function () {
      var c = db._create(cn);
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      c.insert({
        _key: 'x',
        value: 1
      });
      c.insert({
        _key: 'y',
        value: 2
      });
      c.insert({
        _key: 'z',
        value: 3
      });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      assertEqual(3, c.count());
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);
      assertEqual(3, c.document('z').value);

      connectToLeader();
      c = db._collection(cn);
      c.remove('z');
      c.insert({
        _key: 'w',
        value: 3
      });

      assertEqual(3, c.count());
      assertEqual(3, c.document('w').value);
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      c = db._collection(cn);
      assertEqual(3, c.count());
      assertEqual(3, c.document('w').value);
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);

      
      connectToLeader();
      c = db._collection(cn);

      c.remove('w');
      c.insert({
        _key: 'z',
        value: 3
      });
      
      assertEqual(3, c.count());
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);
      assertEqual(3, c.document('z').value);

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      c = db._collection(cn);
      assertEqual(3, c.count());
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);
      assertEqual(3, c.document('z').value);
    },
    
    testKeyConflictsRandom: function () {
      var c = db._create(cn);
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
     
      let keys = [];
      for (let i = 0; i < 1000; ++i) {
        keys.push(internal.genRandomAlphaNumbers(10));
      }

      keys.forEach(function(key, i) {
        c.insert({ _key: key, value: i });
      });

      connectToLeader();
      c = db._collection(cn);
     
      function shuffle(array) {
        for (let i = array.length - 1; i > 0; i--) {
          const j = Math.floor(Math.random() * (i + 1));
          [array[i], array[j]] = [array[j], array[i]];
        }
      }
      shuffle(keys);

      keys.forEach(function(key, i) {
        c.insert({ _key: key, value: i });
      });
      
      assertEqual(1000, c.count());
      let checksum = collectionChecksum(c.name());

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      c = db._collection(cn);
      assertEqual(1000, c.count());
      assertEqual(checksum, collectionChecksum(c.name()));
    },
    
    testKeyConflictsRandomDiverged: function () {
      var c = db._create(cn);
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
     
      for (let i = 0; i < 1000; ++i) {
        c.insert({ _key: internal.genRandomAlphaNumbers(10), value: i });
      }

      connectToLeader();
      c = db._collection(cn);
      
      for (let i = 0; i < 1000; ++i) {
        c.insert({ _key: internal.genRandomAlphaNumbers(10), value: i });
      }
     
      assertEqual(1000, c.count());
      let checksum = collectionChecksum(c.name());

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      c = db._collection(cn);
      assertEqual(1000, c.count());
      assertEqual(checksum, collectionChecksum(c.name()));
    },
    
    testKeyConflictsIncrementalManyDocuments: function () {
      var c = db._create(cn);
      var i;
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      for (i = 0; i < 10000; ++i) {
        c.insert({
          _key: 'test' + i,
          value: i
        });
      }

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);

      assertEqual(10000, c.count());

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      connectToLeader();
      c = db._collection(cn);

      c.remove('test0');
      c.remove('test1');
      c.remove('test9998');
      c.remove('test9999');

      c.insert({
        _key: 'test0',
        value: 9999
      });
      c.insert({
        _key: 'test1',
        value: 9998
      });
      c.insert({
        _key: 'test9998',
        value: 1
      });
      c.insert({
        _key: 'test9999',
        value: 0
      });

      assertEqual(10000, c.count());

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      c = db._collection(cn);
      assertEqual(10000, c.count());

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);
    }
  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite for key conflicts in non-incremental sync
// //////////////////////////////////////////////////////////////////////////////

function ReplicationNonIncrementalKeyConflict () {
  'use strict';

  return {

    setUp: function () {
      connectToLeader();
      db._drop(cn);
    },

    tearDown: function () {
      connectToLeader();
      db._drop(cn);
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });

      connectToFollower();
      db._drop(cn);
      db._flushCache();
      userManager.all().forEach(user => {
        if (user.user !== "root") {
          userManager.remove(user.user);
        }
      });
    },

    testKeyConflictsNonIncremental: function () {
      var c = db._create(cn);
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      c.insert({
        _key: 'x',
        value: 1
      });
      c.insert({
        _key: 'y',
        value: 2
      });
      c.insert({
        _key: 'z',
        value: 3
      });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);

      assertEqual(3, c.count());
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);
      assertEqual(3, c.document('z').value);

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      connectToLeader();
      c = db._collection(cn);
      c.remove('z');
      c.insert({
        _key: 'w',
        value: 3
      });

      assertEqual(3, c.count());
      assertEqual(3, c.document('w').value);
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: false,
        skipCreateDrop: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      c = db._collection(cn);
      assertEqual(3, c.count());
      assertEqual(3, c.document('w').value);
      assertEqual(1, c.document('x').value);
      assertEqual(2, c.document('y').value);

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);
    },
    
    testKeyConflictsNonIncrementalManyDocuments: function () {
      var c = db._create(cn);
      var i;
      c.ensureIndex({
        type: 'hash',
        fields: ['value'],
        unique: true
      });

      for (i = 0; i < 10000; ++i) {
        c.insert({
          _key: 'test' + i,
          value: i
        });
      }

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);

      assertEqual(10000, c.count());

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);

      connectToLeader();
      c = db._collection(cn);

      c.remove('test0');
      c.remove('test1');
      c.remove('test9998');
      c.remove('test9999');

      c.insert({
        _key: 'test0',
        value: 9999
      });
      c.insert({
        _key: 'test1',
        value: 9998
      });
      c.insert({
        _key: 'test9998',
        value: 1
      });
      c.insert({
        _key: 'test9999',
        value: 0
      });

      assertEqual(10000, c.count());

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: false,
        skipCreateDrop: true,
        username: "root",
        password: "",
      });

      db._flushCache();

      c = db._collection(cn);
      assertEqual(10000, c.count());

      assertEqual('hash', c.indexes()[1].type);
      assertTrue(c.indexes()[1].unique);
    }
  };
}

jsunity.run(ReplicationSuite);
jsunity.run(ReplicationOtherDBTraditionalNameSuite);
jsunity.run(ReplicationOtherDBExtendedNameSuite);
jsunity.run(ReplicationIncrementalKeyConflict);
jsunity.run(ReplicationNonIncrementalKeyConflict);

return jsunity.done();
