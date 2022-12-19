/*jshint globalstrict:false, strict:false */
/* global arango, getOptions, runSetup, assertTrue, assertFalse, assertEqual */

////////////////////////////////////////////////////////////////////////////////
/// @brief test for server startup options
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB Inc, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2019, ArangoDB Inc, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

if (getOptions === true) {
  return {
    'rocksdb.auto-refill-index-caches-on-modify' : 'true',
    'server.statistics' : 'false',
    'foxx.queues' : 'false',
  };
}

const db = require('@arangodb').db;
const jsunity = require('jsunity');
const getMetric = require('@arangodb/test-helper').getMetricSingle;
const time = require('internal').time;

const cn = 'UnitTestsCollection';
const n = 5000;

const waitForMetrics = () => {
  // wait for metrics to settle, using an informal API
  arango.POST('/_api/index/sync-caches', {});
};

function AutoRefillIndexCachesEdge() {
  'use strict';

  let runCheck = (expectHits) => {
    let crsr = db._query(`FOR i IN 0..${n - 1} FOR doc IN ${cn} FILTER doc._from == CONCAT('v/test', i) RETURN doc._from`);
    let res = crsr.toArray();
    assertEqual(n, res.length);
    res.forEach((val, i) => {
      assertEqual(val, 'v/test' + i);
    });
    let stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    } else {
      assertEqual(0, stats.cacheHits, stats);
    }

    crsr = db._query(`FOR i IN 0..24 FOR doc IN ${cn} FILTER doc._to == CONCAT('v/test', i) RETURN doc._to`);
    res = crsr.toArray();
    assertEqual(n, res.length);
    res.forEach((val, i) => {
      assertEqual(val, 'v/test' + Math.floor(i / (n / 25)));
    });
    stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    } else {
      assertEqual(0, stats.cacheHits, stats);
    }
  };
  
  let runRemoveCheck = (expectHits) => {
    let crsr = db._query(`FOR i IN 0..${n - 1} FOR doc IN ${cn} FILTER doc._from == CONCAT('v/test', i) RETURN doc._from`);
    let res = crsr.toArray();
    assertTrue(res.length > 0, res.length);
    let stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    }

    crsr = db._query(`FOR i IN 0..24 FOR doc IN ${cn} FILTER doc._to == CONCAT('v/test', i) RETURN doc._to`);
    res = crsr.toArray();
    assertTrue(res.length > 0, res.length);
    stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    }
  };

  return {
    setUp: function() {
      db._createEdgeCollection(cn);
    },

    tearDown: function() {
      db._drop(cn);
    },
    
    testInsertEdgeEnabled: function() {
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(true);
    },
    
    testInsertEdgeDisabled: function() {
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(false);
    },
    
    testUpdateEdgeEabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} UPDATE doc WITH {value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(true);
    },
    
    testUpdateEdgeDisbled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} UPDATE doc WITH {value: doc.value + 1} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(false);
    },
    
    testReplaceEdgeEnabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} REPLACE doc WITH {_from: doc._from, _to: doc._to, value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(true);
    },
    
    testReplaceEdgeDisabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} REPLACE doc WITH {_from: doc._from, _to: doc._to, value: doc.value + 1} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(false);
    },
    
    testRemoveEdgeEnabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_key: CONCAT('test', i), _from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n / 2 - 1} REMOVE CONCAT('test', i) INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n / 2, { oldValue, newValue });
      waitForMetrics();
      runRemoveCheck(true);
    },
    
    testRemoveEdgeDisabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_key: CONCAT('test', i), _from: CONCAT('v/test', i), _to: CONCAT('v/test', (i % 25))} INTO ${cn}`);
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n / 2 - 1} REMOVE CONCAT('test', i) INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runRemoveCheck(false);
    },
    
  };
}

function AutoRefillIndexCachesVPack() {
  'use strict';

  let runCheck = (offset, expectHits) => {
    let crsr = db._query(`FOR i IN ${0 + offset}..${n - 1 + offset} FOR doc IN ${cn} FILTER doc.value == i RETURN doc.value`);
    let res = crsr.toArray();
    assertEqual(n, res.length);
    res.forEach((val, i) => {
      assertEqual(val, i + offset);
    });
    let stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    } else {
      assertEqual(0, stats.cacheHits, stats);
    }
  };
  
  let runRemoveCheck = (expectHits) => {
    let crsr = db._query(`FOR i IN ${0}..${n - 1} FOR doc IN ${cn} FILTER doc.value == i RETURN doc.value`);
    let res = crsr.toArray();
    assertTrue(res.length > 0, res.length);
    let stats = crsr.getExtra().stats;
    if (expectHits) {
      assertTrue(stats.cacheHits > 0, stats);
    } else {
      assertEqual(0, stats.cacheHits, stats);
    }
  };

  return {
    setUp: function() {
      db._create(cn);
    },

    tearDown: function() {
      db._drop(cn);
    },
    
    testInsertNoIndex: function() {
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(0, false);
    },
    
    testInsertVPackNoCache: function() {
      db[cn].ensureIndex({ type: "persistent", fields: ["value"] });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(0, false);
    },
    
    testInsertVPackDisabled: function() {
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      waitForMetrics();
      runCheck(0, false);
    },
    
    testInsertVPackEnabled: function() {
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(0, true);
    },
    
    testUpdateVPackNoCache: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"] });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} UPDATE doc WITH {value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(1, false);
    },
    
    testUpdateVPackDisabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} UPDATE doc WITH {value: doc.value + 1} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      runCheck(1, false);
    },
    
    testUpdateVPackEnabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} UPDATE doc WITH {value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(1, true);
    },
    
    testReplaceVPackNoCache: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"] });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} REPLACE doc WITH {value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      waitForMetrics();
      runCheck(1, false);
    },
    
    testReplaceVPackDisabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} REPLACE doc WITH {value: doc.value + 1} INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      waitForMetrics();
      runCheck(1, false);
    },
    
    testReplaceVPackEnabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR doc IN ${cn} REPLACE doc WITH {value: doc.value + 1} INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n, { oldValue, newValue });
      waitForMetrics();
      runCheck(1, true);
    },
    
    testRemoveVPackNoCache: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_key: CONCAT('test', i), value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"] });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n / 2 - 1} REMOVE CONCAT('test', i) INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      waitForMetrics();
      runRemoveCheck(false);
    },
    
    testRemoveVPackDisabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_key: CONCAT('test', i), value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n / 2 - 1} REMOVE CONCAT('test', i) INTO ${cn} OPTIONS { refillIndexCaches: false }`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      // 10 here because there may be background operations running that
      // could affect the metrics
      assertTrue(newValue - oldValue <= 10, { oldValue, newValue });
      waitForMetrics();
      runRemoveCheck(false);
    },
    
    testRemoveVPackEnabled: function() {
      db._query(`FOR i IN 0..${n - 1} INSERT {_key: CONCAT('test', i), value: i} INTO ${cn}`);
      db[cn].ensureIndex({ type: "persistent", fields: ["value"], cacheEnabled: true });
      const oldValue = getMetric("rocksdb_cache_auto_refill_queued_total");
      db._query(`FOR i IN 0..${n / 2 - 1} REMOVE CONCAT('test', i) INTO ${cn}`);
      const newValue = getMetric("rocksdb_cache_auto_refill_queued_total");

      assertTrue(newValue - oldValue >= n / 2, { oldValue, newValue });
      waitForMetrics();
      runRemoveCheck(true);
    },

  };
}

jsunity.run(AutoRefillIndexCachesEdge);
jsunity.run(AutoRefillIndexCachesVPack);
return jsunity.done();
