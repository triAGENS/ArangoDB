/* jshint globalstrict:false, strict:false, unused: false */
/* global arango, assertEqual, assertTrue, ARGUMENTS, fail */

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
const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
const db = arangodb.db;
const replication = require('@arangodb/replication');
const errors = require("internal").errors;
const { deriveTestSuite, compareStringIds, versionHas } = require('@arangodb/test-helper');
const _ = require('lodash');
let IM = global.instanceManager;
const leaderEndpoint = IM.arangods[0].endpoint;
const followerEndpoint = IM.arangods[1].endpoint;

const cn = 'UnitTestsReplication';

const docCount = versionHas('tsan') ? 10000 : 100000;

const connectToLeader = function() {
  reconnectRetry(IM.arangods[0].endpoint, db._name(), 'root', '');
  db._flushCache();
};

const connectToFollower = function() {
  reconnectRetry(IM.arangods[1].endpoint, db._name(), 'root', '');
  db._flushCache();
};

const setFailurePoint = function(which) {
  let res = arango.PUT("/_admin/debug/failat/" + encodeURIComponent(which), {});
  if (res !== true) {
    throw "unable to set failure point '" + which + "'";
  }
};

const clearFailurePoints = function() {
  arango.DELETE("/_admin/debug/failat");
};

function BaseTestConfig () {
  'use strict';
  
  let checkCountConsistency = function(cn, expected) {
    let check = function() {
      db._flushCache();
      let c = db[cn];
      let figures = c.figures(true).engine;

      // count returns a stored value, and doesn't iterate over the documents in the collection
      assertEqual(expected, c.count(), figures);
      // iterate the documents in the collection, but don't return them in full (too expensive)
      assertEqual(expected, db._query(`FOR doc IN ${c.name()} RETURN 1`).toArray().length, figures);
      assertEqual(expected, figures.documents, figures);
      assertEqual("primary", figures.indexes[0].type, figures);
      figures.indexes.forEach((idx) => {
        assertEqual(expected, idx.count, figures);
      });
    };

    connectToFollower();
    check();
    connectToLeader();
    check();
  };
  
  let runRandomOps = function(cn, n) {
    let state = {};
    let c = db[cn];
    for (let i = 0; i < n; ++i) {
      let key = "testmann" + Math.floor(Math.random() * 10);
      if (state.hasOwnProperty(key)) {
        if (Math.random() >= 0.666) {
          // remove
          c.remove(key, { silent: true });
          delete state[key];
        } else {
          c.replace(key, { value: ++state[key] }, { silent: true });
        }
      } else {
        state[key] = Math.floor(Math.random() * 10000);
        c.insert({ _key: key, value: state[key] }, { silent: true });
      }
    };

    return state;
  };

  let runRandomOpsTransaction = function(cn, n) {
    return db._executeTransaction({
      collections: { write: cn },
      action: function(params) {
        let db = require("@arangodb").db;
        let state = {};
        let c = db[params.cn];
        for (let i = 0; i < params.n; ++i) {
          let key = "testmann" + Math.floor(Math.random() * 10);
          if (state.hasOwnProperty(key)) {
            if (Math.random() >= 0.666) {
              // remove
              c.remove(key, { silent: true });
              delete state[key];
            } else {
              c.replace(key, { value: ++state[key] }, { silent: true });
            }
          } else {
            state[key] = Math.floor(Math.random() * 10000);
            c.insert({ _key: key, value: state[key] }, { silent: true });
          }
        };

        return state;
      },
      params: { cn, n }
    });
  };

  return {

    tearDown: function () {
      connectToFollower();
      // clear all failure points
      clearFailurePoints();
      db._drop(cn);

      connectToLeader();
      // clear all failure points
      clearFailurePoints();
      db._drop(cn);
    },
    
    testInsertOldRevisions: function () {
      let c = db._create(cn);
      let rev1 = c.insert({ _key: "a" })._rev;
      
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });
      
      connectToLeader();
      db._flushCache();
      
      // insert an older revision
      let rev2 = c.insert({ _key: "b", _rev: "_ZAAHwuy---" }, { isRestore: true })._rev;
      assertEqual("_ZAAHwuy---", rev2);

      assertEqual(1, compareStringIds(rev1, rev2));

      connectToFollower();
      db._flushCache();
      
      // sync again
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      c = db._collection(cn);
     
      checkCountConsistency(cn, 2);
    },

    testIndexConflicts: function () {
      let c = db._create(cn);
      c.ensureIndex({ type: "persistent", fields: ["value"], unique: true });

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      let docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: 9999 - i });
      }
      c.insert(docs);

      connectToLeader();
      docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: i });
      }
      c.insert(docs);
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 10000);
    },
    
    testMoreOnLeader: function () {
      let c = db._create(cn);

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      let docs = [];
      for (let i = 19; i < 10000; i += 111) {
        docs.push({ _key: "test" + i, value: "follower-" + i });
      }
      c.insert(docs);

      connectToLeader();
      docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: "leader-" + i });
      }
      c.insert(docs);
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 10000);
    },
    
    testMoreOnFollower: function () {
      let c = db._create(cn);

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      let docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: "follower-" + i });
      }
      c.insert(docs);

      connectToLeader();
      docs = [];
      for (let i = 19; i < 10000; i += 111) {
        docs.push({ _key: "test" + i, value: "leader-" + i });
      }
      c.insert(docs);
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 90);
    },
    
    testDifferentDocuments: function () {
      let c = db._create(cn);

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      let docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: "follower-" + i });
      }
      c.insert(docs);

      connectToLeader();
      docs = [];
      for (let i = 0; i < 10000; ++i) {
        docs.push({ _key: "test" + i, value: "leader-" + i });
      }
      c.insert(docs);
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 10000);
    },
    
    testLargeDocuments: function () {
      let c = db._create(cn);

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });
      // insert 1 document, so that the incremental sync gets triggered
      c.insert({ _key: "testi" });
      assertEqual(1, c.count());

      connectToLeader();
      let payload = Array(512).join("x");
      let doc = {};
      for (let i = 0; i < 80; ++i) {
        doc["test" + i] = payload;
      }
      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push(doc);
      }
      for (let i = 0; i < 10000; ++i) {
        c.insert(docs);
        i += docs.length;
      }
      assertEqual(10000, c.count());
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 10000);
    },
    
    testLargeDocumentsWithEvilKeys: function () {
      let c = db._create(cn);

      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });
      // insert documents with evil keys
      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ _key: "testi$!%20%44+abc=" + i });
      }
      c.insert(docs);
      assertEqual(100, c.count());

      connectToLeader();
      let payload = Array(512).join("x");
      let doc = {};
      for (let i = 0; i < 100; ++i) {
        doc["test" + i] = payload;
      }
      docs = [];
      for (let i = 0; i < 10000; ++i) {
        doc._key = "testi$!%20%44+abc=" + i;
        docs.push(_.clone(doc));
        if (docs.length === 100) {
          c.insert(docs);
          docs = [];
        }
      }
      assertEqual(10000, c.count());
      
      connectToFollower();

      // sync all documents
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      checkCountConsistency(cn, 10000);
    },
    
    
    testInsertRemoveInsertRemove: function () {
      let c = db._create(cn);
      // create collection on follower too
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      connectToLeader();
      db._flushCache();
      let rev1 = c.insert({ _key: "a" })._rev;
      c.remove("a");
      let rev2 = c.insert({ _rev: rev1, _key: "a" }, { isRestore: true })._rev;
      c.remove("a");

      assertEqual(rev1, rev2);

      connectToFollower();
      db._flushCache();
      
      rev1 = c.insert({ _rev: rev1, _key: "a" }, { isRestore: true })._rev;
      assertEqual(rev1, c.document("a")._rev);
      rev2 = c.insert({ _key: "b" })._rev;
      assertEqual(rev2, c.document("b")._rev);

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);
     
      checkCountConsistency(cn, 0);
    },
    
    testInsertRemoveInsertRemoveInsert: function () {
      let c = db._create(cn);
      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      connectToLeader();
      db._flushCache();
      let rev1 = c.insert({ _key: "a" })._rev;
      c.remove("a");
      let rev2 = c.insert({ _rev: rev1, _key: "a" }, { isRestore: true })._rev;
      c.remove("a");
      let rev3 = c.insert({ _rev: rev1, _key: "a" }, { isRestore: true })._rev;

      assertEqual(rev1, rev2);
      assertEqual(rev1, rev3);

      connectToFollower();
      db._flushCache();
      
      assertEqual(rev1, c.insert({ _rev: rev1, _key: "a" }, { isRestore: true })._rev);
      assertEqual(rev1, c.document("a")._rev);
      rev2 = c.insert({ _key: "b" })._rev;
      assertEqual(rev2, c.document("b")._rev);

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);
     
      assertEqual(rev1, c.document("a")._rev);
     
      checkCountConsistency(cn, 1);
    },

    testSecondaryIndexConflictsDocuments: function () {
      let c = db._create(cn);
      c.ensureIndex({ type: "hash", fields: ["value"], unique: true });

      connectToFollower();
      db._flushCache();

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        username: "root",
        password: "",
      });

      connectToLeader();
      db._flushCache();

      c.insert({ _key: "a", value: 1 });
      c.insert({ _key: "b", value: 2 });
      c.insert({ _key: "c", value: 3 });
      
      assertEqual(1, c.document("a").value);
      assertEqual(2, c.document("b").value);
      assertEqual(3, c.document("c").value);

      connectToFollower();
     
      assertEqual(2, c.indexes().length);
      c.insert({ _key: "b", value: 3 });
      c.insert({ _key: "c", value: 2 });

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);
     
      assertEqual(1, c.document("a").value);
      assertEqual(2, c.document("b").value);
      assertEqual(3, c.document("c").value);
     
      checkCountConsistency(cn, 3);
    },
    
    // create different state on follower
    testDowngradeManyRevisions: function () {
      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ value: i });
        if (docs.length === 10000) {
          c.insert(docs, { silent: true });
          docs = [];
        }
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
     
      db._query("FOR doc IN " + cn + " REPLACE doc WITH { value: doc.value + 1 } IN " + cn);

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);

      checkCountConsistency(cn, docCount);
    },
    
    // create different state on follower
    testRewriteEntireFollowerSecondaryUniqueIndexes: function () {
      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ value: i });
        if (docs.length === 10000) {
          c.insert(docs, { silent: true });
          docs = [];
        }
      }

      c.ensureIndex({ type: "hash", fields: ["value"], unique: true });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
     
      db._query("FOR doc IN " + cn + " REPLACE doc WITH { value: doc.value + 1000000 } IN " + cn);
      
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);

      checkCountConsistency(cn, docCount);
    },
    
    testUniqueConstraints: function () {
      let c = db._create(cn);
      c.ensureIndex({type: "hash", unique: true, fields: ["x"]});
      c.ensureIndex({type: "hash", unique: true, fields: ["y"]});
      c.ensureIndex({type: "hash", unique: true, fields: ["z"]});

      let docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "K" + i, value: i, x: i, y: i, z: i });
        if (docs.length === 10000) {
          c.insert(docs, { silent: true });
          docs = [];
        }
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
     
      db._query("FOR doc IN " + cn + " SORT doc.value REPLACE doc WITH { value: doc.value - 1, x: doc.x + 1000000, y: doc.y + 1000000, z: doc.z + 1000000} IN " + cn);
      
      docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "L" + i, value: i, x: i, y: i, z: i });
        if (docs.length === 10000) {
          c.insert(docs, { silent: true });
          docs = [];
        }
      }

      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);

      checkCountConsistency(cn, docCount);

      // Now check that the unique index entries are all in place by
      // provoking violations:
      docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", x: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", y: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", z: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }
      // And use the index entries in a query:
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn}
             FILTER doc.x == i 
             RETURN 1)`).toArray()[0]);
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn}
             FILTER doc.y == i 
             RETURN 1)`).toArray()[0]);
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn} 
             FILTER doc.z == i 
             RETURN 1)`).toArray()[0]);
    },
    
    testManyUniqueConstraints: function () {
      let c = db._create(cn);
      c.ensureIndex({type: "hash", unique: true, fields: ["x"]});
      c.ensureIndex({type: "hash", unique: true, fields: ["y"]});
      c.ensureIndex({type: "hash", unique: true, fields: ["z"]});

      let docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "K" + i, value: i, x: i, y: i, z: i });
        if (docs.length === 10000) {
          c.insert(docs, { silent: true });
          docs = [];
        }
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
     
      db._query("FOR doc IN " + cn + " SORT doc.value REPLACE doc WITH { value: doc.value - 1, x: doc.x - 1, y: doc.y - 2, z: doc.z - 3 } IN " + cn);
      
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        incremental: true,
        username: "root",
        password: "",
      });

      db._flushCache();
      c = db._collection(cn);

      checkCountConsistency(cn, docCount);

      // Now check that the unique index entries are all in place by
      // provoking violations:
      docs = [];
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", x: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }

      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", y: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }
      
      for (let i = 0; i < docCount; ++i) {
        docs.push({ _key: "N", z: i });
        if (docs.length === 10000) {
          let res = c.insert(docs);
          assertEqual(10000, res.length);
          res.forEach((r) => {
            assertTrue(r.error);
            assertEqual(errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code,
                        r.errorNum);
          });
          docs = [];
        }
      }

      // And use the index entries in a query:
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn} 
             FILTER doc.x == i 
             RETURN 1)`).toArray()[0]);
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn} 
             FILTER doc.y == i 
             RETURN 1)`).toArray()[0]);
      assertEqual(docCount, db._query(
        `RETURN COUNT(FOR i IN 0..99999
           FOR doc IN ${cn} 
             FILTER doc.z == i 
             RETURN 1)`).toArray()[0]);
    },
    
    testRevisionIdReuse: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
    
      // document on follower must be 100% identical
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      c.remove("testi");
      c.insert({_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);
      c.remove("testi");

      setFailurePoint("Insert::useRev");
      c.insert({ _key: "testi", value: 1, _rev: rev });

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
      // document on follower must be 100% identical
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      checkCountConsistency(cn, 1);
    },
   
    // create different state on follower for the same key
    testDifferentKeyStateOnFollower: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
     
      // document on follower must be 100% identical
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      c.remove("testi");
      c.insert({_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);
      c.remove("testi");
      rev = c.insert({ _key: "testi", value: 3 })._rev;

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
      assertEqual(3, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      checkCountConsistency(cn, 1);
    },
    
    // create different state on follower for the same key, using replace
    testDifferentKeyStateOnFollowerUsingReplace: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
     
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      c.replace("testi", {_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);
      rev = c.replace("testi", { _key: "testi", value: 3 })._rev;

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
      assertEqual(3, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);
      
      checkCountConsistency(cn, 1);
    },
      
    // create large AQL operation on leader using AQL
    testCreateLargeAQLOnLeader: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);
      
      c.replace("testi", {_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      setFailurePoint("TransactionState::intermediateCommitCount1000");
      db._query("FOR i IN 1..10000 INSERT { _key: CONCAT('testmann', i) } INTO " + cn);
      
      rev = c.replace("testi", { _key: "testi", value: 3 })._rev;

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
      assertEqual(3, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      checkCountConsistency(cn, 10001);
    },
    
    // create large AQL operation on follower using AQL
    testCreateLargeAQLOnFollower: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);
      
      c.replace("testi", {_key: "testi", value: 2 });
      
      setFailurePoint("TransactionState::intermediateCommitCount1000");
      db._query("FOR i IN 1..10000 INSERT { _key: CONCAT('testmann', i) } INTO " + cn);

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      rev = c.replace("testi", { _key: "testi", value: 3 })._rev;

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
      assertEqual(3, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      checkCountConsistency(cn, 1);
    },
    
    // create random operations on leader, with many
    // operations affected the same keys
    testCreateRandomOperationsOnLeader: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      let state = runRandomOps(cn, 50000);
      
      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      let expected = 1;
      for (let s in state) {
        ++expected;
        assertEqual(state[s], c.document(s).value);
      }
      
      checkCountConsistency(cn, expected);
    },
    
    // create a transaction with random operations on leader, with many
    // operations affected the same keys
    testCreateRandomOperationsTransactionOnLeader: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      let state = runRandomOpsTransaction(cn, 50000);
      
      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      let expected = 1;
      for (let s in state) {
        ++expected;
        assertEqual(state[s], c.document(s).value);
      }
      
      checkCountConsistency(cn, expected);
    },
    
    // create random operations on follower, with many
    // operations affected the same keys
    testCreateRandomOperationsOnFollower: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });
      
      runRandomOps(cn, 50000);

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      checkCountConsistency(cn, 1);
    },
    
    // create a transaction with random operations on follower, with many
    // operations affected the same keys
    testCreateRandomOperationsTransactionOnFollower: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });
      
      runRandomOpsTransaction(cn, 50000);

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      checkCountConsistency(cn, 1);
    },
    
    // create random operations on leader and follower, with many
    // operations affected the same keys
    testCreateRandomOperationsOnLeaderAndFollower: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });
      
      runRandomOps(cn, 50000);

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      let state = runRandomOps(cn, 50000);
      
      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      let expected = 1;
      for (let s in state) {
        ++expected;
        assertEqual(state[s], c.document(s).value);
      }
      
      checkCountConsistency(cn, expected);
    },
    
    // create a transaction with random operations on leader and follower, with many
    // operations affected the same keys
    testCreateRandomOperationsTransactionOnLeaderAndFollower: function () {
      let c = db._create(cn);
      c.insert({_key: "testi", value: 1 });

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);
      
      assertEqual(1, c.document("testi").value);
      
      c.replace("testi", {_key: "testi", value: 2 });
      
      runRandomOpsTransaction(cn, 50000);

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      let state = runRandomOpsTransaction(cn, 50000);
      
      c.replace("testi", { _key: "testi", value: 3 });

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
      assertEqual(3, c.document("testi").value);

      let expected = 1;
      for (let s in state) {
        ++expected;
        assertEqual(state[s], c.document(s).value);
      }
      
      checkCountConsistency(cn, expected);
    }, 
    // create large AQL operation on leader using AQL
    testIncrementalQuickKeys: function () {
      let c = db._create(cn);
      let rev = c.insert({_key: "testi", value: 1 })._rev;

      connectToFollower();
      replication.syncCollection(cn, {
        endpoint: leaderEndpoint,
        verbose: true,
        username: "root",
        password: "",
      });
      db._flushCache();
      c = db._collection(cn);

      assertEqual(1, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      c.replace("testi", {_key: "testi", value: 2 });

      connectToLeader();
      db._flushCache();
      c = db._collection(cn);

      setFailurePoint("RocksDBRestReplicationHandler::quickKeysNumDocsLimit100");
      db._query("FOR i IN 1..10000 INSERT { _key: CONCAT('testmann', i) } INTO " + cn);

      rev = c.replace("testi", { _key: "testi", value: 3 })._rev;

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
      assertEqual(3, c.document("testi").value);
      assertEqual(rev, c.document("testi")._rev);

      checkCountConsistency(cn, 10001);
    },
  };
}

exports.BaseTestConfig = BaseTestConfig;

exports.connectToLeader = function () {
  reconnectRetry(leaderEndpoint, db._name(), 'root', '');
  db._flushCache();
};

exports.connectToFollower = function () {
  reconnectRetry(followerEndpoint, db._name(), 'root', '');
  db._flushCache();
};
  
exports.setFailurePoint = function(which) {
  let res = arango.PUT("/_admin/debug/failat/" + encodeURIComponent(which), {});
  if (res !== true) {
    throw "unable to set failure point '" + which + "'";
  }
};

exports.clearFailurePoints = function() {
  arango.DELETE("/_admin/debug/failat");
};
