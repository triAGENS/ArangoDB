/*jshint globalstrict:false, strict:false, maxlen:4000 */
/*global fail, assertEqual, assertNotEqual, assertTrue, assertFalse, assertNull, assertTypeOf, assertUndefined, arango, print */

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
/// @author Jan Steemann
/// @author Wilfried Goesgens
/// @author Copyright 2021, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

(function () {
  const _ = require('lodash');
  const fs = require('fs');
  const arangodb = require("@arangodb");
  const internal = require("internal");
  const jsunity = require("jsunity");
  const users = require("@arangodb/users");
  const analyzers = require("@arangodb/analyzers");
  const arangosh = require('@arangodb/arangosh');
  const isCluster = internal.isCluster();
  const isEnterprise = internal.isEnterprise();
  const instanceInfo = JSON.parse(require('internal').env.INSTANCEINFO);
  var db = internal.db;
  const getNoShards = function (defaultNo) {
    if (db._properties().sharding === "single") {
      return 1;
    }
    return defaultNo;
  };

  const getDumpDir = () => {
    let dumpDir = instanceInfo.rootDir;
    if (fs.exists(fs.join(dumpDir, "UnitTestsDumpSrc"))) {
      // when dumping multiple database the database name is one layer added:
      dumpDir = fs.join(dumpDir, "UnitTestsDumpSrc");
    } else {
      // single database always ends up in "dump"
      dumpDir = fs.join(dumpDir, "dump");
    }
    return dumpDir;
  };

  const getDumpDir = () => {
    let dumpDir = instanceInfo.rootDir;
    if (fs.exists(fs.join(dumpDir, "UnitTestsDumpSrc"))) {
      // when dumping multiple database the database name is one layer added:
      dumpDir = fs.join(dumpDir, "UnitTestsDumpSrc");
    } else {
      // single database always ends up in "dump"
      dumpDir = fs.join(dumpDir, "dump");
    }
    return dumpDir;
  };

  function getCollection(name) {
    var c = db._collection(name);
    if (c === null) {
      let allCols = db._collections().map(c => c.name());
      assertNotEqual(c, null, `${name} not found, ${JSON.stringify(allCols)} are here`);
    }
    return c;
  }

  function waitForCollInSync(collName) {
    let res;
    let numSecs = 0.5;
    let collDistribution = {};
    let collDistributionPlan = {};
    let collDistributionCurrent = {};
    const isCov = require("@arangodb/test-helper").versionHas('coverage');
    const numSecsLimit = (isCov)? 64:16;
    while (true) {
      const dbName = db._name();
      const res = db._connection.GET("/_db/" + dbName + "/_admin/cluster/shardDistribution");
      const colls = res["results"];
      for (const key in colls) {
        if (key === collName) {
          collDistribution = colls[key];
          break;
        }
      }
      assertFalse(_.isEmpty(collDistribution));
      collDistributionPlan = collDistribution["Plan"];
      collDistributionCurrent = collDistribution["Current"];
      assertFalse(_.isEmpty(collDistributionPlan), {collName, res});
      assertFalse(_.isEmpty(collDistributionCurrent), {collName, res});
      if (_.isEqual(collDistributionPlan, collDistributionCurrent) || numSecs >= numSecsLimit) {
        break;
      }
      internal.sleep(numSecs);
      numSecs *= 2;
    }
    assertTrue(numSecs < numSecsLimit, "reached timeout waited for collection " + collName + " to get in sync");
  }

  const extendedName = "Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨";

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief test suite
  ////////////////////////////////////////////////////////////////////////////////

  return function (args = {
    // testEmpty
    emptyIndexes: 1,
    // testMany
    manyIndexes: 1,
    manyCountStart: 0,
    manyCount: 100000,
    manyCountInc: 1,
    // testEdges
    edgesIndexCount: 2,
    edgeIndexLoopCount: 10,
    edgeIndexLoopInc: 1,
    // testRemoved
    removedCount: 9000,
    removedSingleIndices: 1,
    removedModulo: 10,
    // testIndexes
    indexesCount: 9,
    // testKeygenAutoInc
    keygenAutoInc: 42049,
    autoIncDocCount: 1001,
    // testKeygenPadded
    paddedDocCount: 1001,
    // testKeygenUuid
    uuidDocCount: 1001,
  }) {
    // 'use strict';
    const smartGraphName = "UnitTestDumpSmartGraph";
    const edges = "UnitTestDumpSmartEdges";
    const vertices = "UnitTestDumpSmartVertices";
    const orphans = "UnitTestDumpSmartOrphans";
    const satellite = "UnitTestDumpSatelliteCollection";
    const satelliteGraphName = "UnitTestDumpSatelliteGraph";
    const satelliteVertexCollection1Name = "UnitTestDumpSatelliteVertexCollection1";
    const satelliteVertexCollection2Name = "UnitTestDumpSatelliteVertexCollection2";
    const satelliteOrphanCollectionName = "UnitTestDumpSatelliteOrphanCollection";
    const satelliteEdgeCollection1Name = "UnitTestDumpSatelliteEdgeCollection1";
    const satelliteEdgeCollection2Name = "UnitTestDumpSatelliteEdgeCollection2";
    const gm = (isEnterprise) ? require("@arangodb/smart-graph") : {};
    const satgm = (isEnterprise) ? require("@arangodb/satellite-graph") : {};
    const {map, zipObject} = require('lodash');
    const isServer = require("@arangodb").isServer;
    const request = require("@arangodb/request");
    const {getRawMetric, getEndpointsByType} = require("@arangodb/test-helper");

    console.info("Test suite arguments: " + JSON.stringify(args));

    function assertCommonSmartGraphProperties(graph) {
      assertTrue(graph.isSmart);
      assertFalse(graph.isSatellite);
      assertEqual(graph.replicationFactor, 2);
      assertEqual(graph.numberOfShards, 5);
      assertEqual(graph.smartGraphAttribute, 'value');
    }

    function assertCommonEnterpriseGraphProperties(graph) {
      assertTrue(graph.isSmart);
      assertFalse(graph.isSatellite);
      assertEqual(graph.replicationFactor, 2);
      assertEqual(graph.numberOfShards, 5);
      assertFalse(graph.smartGraphAttribute);
    }

    function assertCommonSatelliteGraphProperties(graph) {
      assertTrue(graph.isSatellite);
      assertFalse(graph.isSmart);
      assertEqual(graph.replicationFactor, "satellite");
      assertEqual(graph.numberOfShards, 1);
    }

    function assertEmptyness(graph) {
      assertEqual(graph.edgeDefinitions.length, 0);
      assertEqual(graph.orphanCollections.length, 0);
    }

    function assertSmartCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, isDisjoint) {
      assertEqual(graph.edgeDefinitions, [
        {
          "collection": edgeCollName,
          "from": [vertexCollName],
          "to": [vertexCollName]
        }
      ]);
      assertEqual(graph.orphanCollections, [orphanCollName]);

      for (const collName of [vertexCollName, orphanCollName]) {
        const vColl = getCollection(collName);
        const vCollProperties = vColl.properties();

        assertTrue(vCollProperties.isSmart);
        assertEqual(vCollProperties.isDisjoint, isDisjoint);
        assertEqual(vCollProperties.smartGraphAttribute, 'value');
        assertEqual(vCollProperties.numberOfShards, 5);
        assertEqual(vCollProperties.replicationFactor, 2);
        if (isCluster) {
          assertEqual(vCollProperties.shardingStrategy, 'hash');
        }
      }

      const eColl = getCollection(edgeCollName);
      const properties = eColl.properties();

      assertTrue(properties.isSmart);
      assertEqual(properties.isDisjoint, isDisjoint);

      assertEqual(properties.replicationFactor, 2);
      if (isCluster) {
        assertEqual(properties.shardingStrategy, 'enterprise-hash-smart-edge');
        assertEqual(properties.numberOfShards, 0);

        const eCollNameLocal = `_local_${edgeCollName}`;
        const eCollNameFrom = `_from_${edgeCollName}`;
        const eCollNameTo = `_to_${edgeCollName}`;
        assertEqual(getCollection(eCollNameLocal).properties().shardingStrategy, 'hash');
        if (!isDisjoint) {
          assertEqual(getCollection(eCollNameFrom).properties().shardingStrategy, 'hash');
          assertEqual(getCollection(eCollNameTo).properties().shardingStrategy, 'hash');
        }

      } else {
        assertEqual(properties.numberOfShards, 5);
      }
    }

    function assertEnterpriseCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, isDisjoint) {
      assertEqual(graph.edgeDefinitions, [
        {
          "collection": edgeCollName,
          "from": [vertexCollName],
          "to": [vertexCollName]
        }
      ]);
      assertEqual(graph.orphanCollections, [orphanCollName]);

      for (const collName of [vertexCollName, orphanCollName]) {
        const vColl = getCollection(collName);
        const properties = vColl.properties();

        assertTrue(properties.isSmart);
        assertEqual(properties.isDisjoint, isDisjoint);
        assertFalse(properties.smartGraphAttribute);
        assertEqual(properties.numberOfShards, 5);
        assertEqual(properties.replicationFactor, 2);
        if (isCluster) {
          assertEqual(properties.shardingStrategy, 'enterprise-hex-smart-vertex');
        }
      }

      const eColl = getCollection(edgeCollName);
      const properties = eColl.properties();

      assertTrue(properties.isSmart);
      assertEqual(properties.isDisjoint, isDisjoint);

      assertEqual(properties.replicationFactor, 2);
      if (isCluster) {
        assertEqual(properties.shardingStrategy, 'enterprise-hash-smart-edge');
        assertEqual(properties.numberOfShards, 0);

        const eCollNameLocal = `_local_${edgeCollName}`;
        const eCollNameFrom = `_from_${edgeCollName}`;
        const eCollNameTo = `_to_${edgeCollName}`;
        assertEqual(getCollection(eCollNameLocal).properties().shardingStrategy, 'hash');
        if (!isDisjoint) {
          assertEqual(getCollection(eCollNameFrom).properties().shardingStrategy, 'hash');
          assertEqual(getCollection(eCollNameTo).properties().shardingStrategy, 'hash');
        }
      } else {
        assertEqual(properties.numberOfShards, 5);
      }
    }

    function assertSatelliteCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName) {
      assertEqual(graph.edgeDefinitions, [
        {
          "collection": edgeCollName,
          "from": [vertexCollName],
          "to": [vertexCollName]
        }
      ]);
      assertEqual(graph.orphanCollections, [orphanCollName]);

      for (const collName of [vertexCollName, orphanCollName]) {
        const vColl = getCollection(collName);
        const properties = vColl.properties();

        assertFalse(properties.isSmart);
        assertFalse(properties.isDisjoint);
        assertEqual(properties.numberOfShards, 1);
        assertEqual(properties.replicationFactor, 'satellite');
        if (isCluster) {
          assertEqual(properties.shardingStrategy, 'hash');
        }
      }

      const eColl = getCollection(edgeCollName);
      const properties = eColl.properties();

      assertFalse(properties.isSmart);
      assertFalse(properties.isDisjoint);
      assertEqual(properties.numberOfShards, 1);
      assertEqual(properties.replicationFactor, 'satellite');
      if (isCluster) {
        assertEqual(properties.shardingStrategy, 'hash');
      }
    }

    return {
      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test the users
      ////////////////////////////////////////////////////////////////////////////////

      testUsers: function () {
        let uName = "foobaruser";

        assertTrue(users.exists(uName));
        assertEqual(users.permission(uName, "_system"), 'rw');
        assertEqual(users.permission(uName, "UnitTestsDumpSrc"), 'rw');
        assertEqual(users.permission(uName, "UnitTestsDumpEmpty"), 'rw');

        assertTrue(users.isValid("foobaruser", "foobarpasswd"));
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test db properties
      ////////////////////////////////////////////////////////////////////////////////

      testDatabaseProperties: function () {
        let old = db._name();
        db._useDatabase("_system");
        try {
          db._useDatabase("UnitTestsDumpProperties1");
          let props = db._properties();
          assertEqual(1, props.replicationFactor);
          assertEqual(1, props.writeConcern);
          db._useDatabase("UnitTestsDumpProperties2");
          props = db._properties();
          assertEqual(2, props.replicationFactor);
          assertEqual(2, props.writeConcern);
        } finally {
          db._useDatabase(old);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test the empty collection
      ////////////////////////////////////////////////////////////////////////////////

      testEmpty: function () {
        var c = getCollection("UnitTestsDumpEmpty");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertTrue(p.waitForSync);

        const indexes = c.indexes();

        assertEqual(args.emptyIndexes, indexes.length, `Found indexes: ${JSON.stringify(indexes)}`); // just primary index
        assertEqual("primary", indexes[0].type);
        assertEqual(0, c.count());
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test the collection with many documents
      ////////////////////////////////////////////////////////////////////////////////

      testMany: function () {
        var c = getCollection("UnitTestsDumpMany");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(args.manyIndexes, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(100000, c.count());

        // test all documents
        let docs = [], results = [];
        for (let i = args.manyCountStart; i < args.manyCount; i += args.manyCountInc) {
          docs.push("test" + i);
          if (docs.length === 10000) {
            results = results.concat(c.document(docs));
            docs = [];
          }
        }
        let j = 0;
        for (let i = args.manyCountStart; i < args.manyCount; i += args.manyCountInc) {
          let doc = results[i];
          if (args.manyCountStart === 1) {
            doc = results[j++];
          }
          assertEqual(i, doc.value1);
          assertEqual("this is a test", doc.value2);
          assertEqual("test" + i, doc.value3);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test the edges collection
      ////////////////////////////////////////////////////////////////////////////////

      testEdges: function () {
        var c = getCollection("UnitTestsDumpEdges");
        var p = c.properties();

        assertEqual(3, c.type()); // edges
        assertFalse(p.waitForSync);

        assertEqual(args.edgesIndexCount, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // primary index + edges index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual("edge", c.indexes()[1].type);
        if (args.edgesIndexCount > 2) {
          // Backup and restore:
          assertEqual("hash", c.indexes()[2].type);
        }
        assertEqual(10, c.count());

        // test all documents
        for (var i = args.edgeIndexLoopStart;
             i < args.edgeIndexLoopCount;
             i += args.edgeIndexLoopInc) {
          var doc = c.document("test" + i);
          assertEqual("test" + i, doc._key);
          assertEqual("UnitTestsDumpMany/test" + i, doc._from);
          assertEqual("UnitTestsDumpMany/test" + (i + 1), doc._to);
          assertEqual(i + "->" + (i + 1), doc.what);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test the order of documents
      ////////////////////////////////////////////////////////////////////////////////

      testOrder: function () {
        var c = getCollection("UnitTestsDumpOrder");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(3, c.count());
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test document removal & update
      ////////////////////////////////////////////////////////////////////////////////

      testRemoved: function () {
        var c = getCollection("UnitTestsDumpRemoved");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(args.removedSingleIndices, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(args.removedCount, c.count());

        for (let i = 0; i < 10000; ++i) {
          if (i % args.removedModulo === 0) {
            assertFalse(c.exists("test" + i));
          } else {
            let doc = c.document("test" + i);
            assertEqual(i, doc.value1);

            if (i < 1000) {
              assertEqual(i + 1, doc.value2);
            }
          }
        }
      },
      
      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test collection with extended name
      ////////////////////////////////////////////////////////////////////////////////
      
      testExtendedName: function () {
        var c = db._collection(extendedName);
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(10000, c.count());

        for (let i = 0; i < 10000; ++i) {
          let doc = c.document("test" + i);
          assertEqual(i, doc.value1);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test indexes
      ////////////////////////////////////////////////////////////////////////////////

      testIndexes: function () {
        var c = getCollection("UnitTestsDumpIndexes");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(args.indexesCount, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`);
        assertEqual("primary", c.indexes()[0].type);

        assertEqual("hash", c.indexes()[1].type);
        assertTrue(c.indexes()[1].unique);
        assertFalse(c.indexes()[1].sparse);
        assertEqual(["a_uc"], c.indexes()[1].fields);

        assertEqual("skiplist", c.indexes()[2].type);
        assertFalse(c.indexes()[2].unique);
        assertFalse(c.indexes()[2].sparse);
        assertEqual(["a_s1", "a_s2"], c.indexes()[2].fields);

        assertEqual("hash", c.indexes()[3].type);
        assertFalse(c.indexes()[3].unique);
        assertFalse(c.indexes()[3].sparse);
        assertEqual(["a_h1", "a_h2"], c.indexes()[3].fields);

        assertEqual("skiplist", c.indexes()[4].type);
        assertTrue(c.indexes()[4].unique);
        assertFalse(c.indexes()[4].sparse);
        assertEqual(["a_su"], c.indexes()[4].fields);

        assertEqual("hash", c.indexes()[5].type);
        assertFalse(c.indexes()[5].unique);
        assertTrue(c.indexes()[5].sparse);
        assertEqual(["a_hs1", "a_hs2"], c.indexes()[5].fields);

        assertEqual("skiplist", c.indexes()[6].type);
        assertFalse(c.indexes()[6].unique);
        assertTrue(c.indexes()[6].sparse);
        assertEqual(["a_ss1", "a_ss2"], c.indexes()[6].fields);

        assertFalse(c.indexes()[7].unique);
        assertEqual("fulltext", c.indexes()[7].type);
        assertEqual(["a_f"], c.indexes()[7].fields);

        assertEqual("geo", c.indexes()[8].type);
        assertEqual(["a_la", "a_lo"], c.indexes()[8].fields);
        assertFalse(c.indexes()[8].unique);

        if (args.indexesCount > 9) {
          assertEqual("hash", c.indexes()[9].type);
          assertEqual(["abc"], c.indexes()[9].fields);
          assertFalse(c.indexes()[9].unique);
        }
        assertEqual(0, c.count());
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test truncate
      ////////////////////////////////////////////////////////////////////////////////

      testTruncated: function () {
        var c = getCollection("UnitTestsDumpTruncated");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(0, c.count());
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test shards
      ////////////////////////////////////////////////////////////////////////////////

      testShards: function () {
        let c = getCollection("UnitTestsDumpShards");
        let p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);
        assertEqual(getNoShards(9), p.numberOfShards);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(1000, c.count());

        for (let i = 0; i < 1000; ++i) {
          let doc = c.document(String(7 + (i * 42)));

          assertEqual(String(7 + (i * 42)), doc._key);
          assertEqual(i, doc.value);
          assertEqual({value: [i, i]}, doc.more);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test computed values
      ////////////////////////////////////////////////////////////////////////////////

      testComputedValues: function () {
        const c = getCollection("UnitTestsComputedValues");
        const p = c.properties();

        assertEqual(p.computedValues.length, 2);
        assertEqual(p.computedValues[0].name, "value3");
        assertEqual(p.computedValues[1].name, "value4");
        assertFalse(p.computedValues[0].overwrite);
        assertTrue(p.computedValues[1].overwrite);
        assertEqual(p.computedValues[0].expression, "RETURN CONCAT(@doc.value1, '+', @doc.value2)");
        assertEqual(p.computedValues[1].expression, "RETURN CONCAT(@doc.value2, ' ', @doc.value1)");

        c.toArray().forEach(el => {
          assertEqual(el.value3, el.value1 + "+" + el.value2);
          assertEqual(el.value4, el.value2 + " " + el.value1);
        });
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test autoinc keygen
      ////////////////////////////////////////////////////////////////////////////////

      testKeygenAutoInc: function () {
        var c = getCollection("UnitTestsDumpKeygen");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);
        assertEqual("autoincrement", p.keyOptions.type);
        if (!isCluster) {
          assertEqual(args.keygenAutoInc, p.keyOptions.lastValue);
        }
        assertFalse(p.keyOptions.allowUserKeys);
        assertEqual(7, p.keyOptions.offset);
        assertEqual(42, p.keyOptions.increment);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(args.autoIncDocCount, c.count());

        for (var i = 0; i < 1000; ++i) {
          var doc = c.document(String(7 + (i * 42)));

          assertEqual(String(7 + (i * 42)), doc._key);
          assertEqual(i, doc.value);
          assertEqual({value: [i, i]}, doc.more);
        }
        if (isCluster) {
          waitForCollInSync("UnitTestsDumpKeygen");
        }
        if (args.autoIncDocCount === 1001) {
          doc = c.save({});
          assertEqual(doc._key, "42091");
        } else if (args.autoIncDocCount === 2001) {
          // hot backup case:
          for (var j = 0; i < 1000; ++i) {
            let doc = c.document(String(7 + ((1000 + j) * 42)));

            assertEqual(String(7 + ((1000 + j) * 42)), doc._key);
            assertEqual(j, doc.value);
            assertEqual({value: [j, j]}, doc.more);
          }
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test padded keygen
      ////////////////////////////////////////////////////////////////////////////////

      testKeygenPadded: function () {
        var c = getCollection("UnitTestsDumpKeygenPadded");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);
        assertEqual("padded", p.keyOptions.type);
        assertFalse(p.keyOptions.allowUserKeys);
        assertEqual(1,  c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(args.paddedDocCount, c.count());

        let allDocs = {};
        let nonDeletedDoc;
        c.toArray().forEach(doc => {
          if (doc.hasOwnProperty('value')) {
            allDocs[doc.value] = doc;
          } else {
            nonDeletedDoc = doc;
          }
        });

        let lastKey = "";
        for (var i = 0; i < 1000; ++i) {
          var doc = allDocs[i];

          assertTrue(doc._key > lastKey, doc._key + ">" + lastKey);
          assertEqual(i, doc.value);
          assertEqual({value: [i, i]}, doc.more);
          lastKey = doc._key;
        }
        if (isCluster) {
          waitForCollInSync("UnitTestsDumpKeygenPadded");
        }
        if (args.paddedDocCount === 1001) {
          assertTrue(nonDeletedDoc._key > lastKey, nonDeletedDoc._key + ">" + lastKey);
          lastKey = nonDeletedDoc._key;
          nonDeletedDoc = c.save({});
          assertTrue(nonDeletedDoc._key > lastKey, nonDeletedDoc._key + ">" + lastKey);
        } else {
          // Backup and restore case:
          doc = c.save({});
          assertTrue(doc._key > lastKey, doc._key + ">" + lastKey);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test uuid keygen
      ////////////////////////////////////////////////////////////////////////////////

      testKeygenUuid: function () {
        var c = getCollection("UnitTestsDumpKeygenUuid");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);
        assertEqual("uuid", p.keyOptions.type);
        assertFalse(p.keyOptions.allowUserKeys);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(args.uuidDocCount, c.count());

        let allDocs = {};
        c.toArray().forEach(doc => {
          allDocs[doc.value] = doc;
        });

        let docs = [];
        for (var i = 0; i < 1000; ++i) docs.push({"a": i});

        if (isCluster) {
          waitForCollInSync("UnitTestsDumpKeygenUuid");
        }
        let savedDocs = c.save(docs);
        savedDocs.forEach(doc => {
          assertFalse(allDocs.hasOwnProperty(doc._key), "found " + doc._key + "!");
        });
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test strings
      ////////////////////////////////////////////////////////////////////////////////

      testStrings: function () {
        var c = getCollection("UnitTestsDumpStrings");
        var p = c.properties();

        assertEqual(2, c.type()); // document
        assertFalse(p.waitForSync);

        assertEqual(1, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`); // just primary index
        assertEqual("primary", c.indexes()[0].type);
        assertEqual(8, c.count());

        var texts = [
          "big. Really big. He moment. Magrathea! - insisted Arthur, - I do you can sense no further because it doesn't fit properly. In my the denies faith, and the atmosphere beneath You are not cheap He was was his satchel. He throughout Magrathea. - He pushed a tore the ecstatic crowd. Trillian sat down the time, the existence is it? And he said, - What they don't want this airtight hatchway. - it's we you shooting people would represent their Poet Master Grunthos is in his mind.",
          "Ultimo cadere chi sedete uso chiuso voluto ora. Scotendosi portartela meraviglia ore eguagliare incessante allegrezza per. Pensava maestro pungeva un le tornano ah perduta. Fianco bearmi storia soffio prende udi poteva una. Cammino fascino elisire orecchi pollici mio cui sai sul. Chi egli sino sei dita ben. Audace agonie groppa afa vai ultima dentro scossa sii. Alcuni mia blocco cerchi eterno andare pagine poi. Ed migliore di sommesso oh ai angoscia vorresti.",
          "Νέο βάθος όλα δομές της χάσει. Μέτωπο εγώ συνάμα τρόπος και ότι όσο εφόδιο κόσμου. Προτίμηση όλη διάφορους του όλο εύθραυστη συγγραφής. Στα άρα ένα μία οποία άλλων νόημα. Ένα αποβαίνει ρεαλισμού μελετητές θεόσταλτο την. Ποντιακών και rites κοριτσάκι παπούτσια παραμύθια πει κυρ.",
          "Mody laty mnie ludu pole rury Białopiotrowiczowi. Domy puer szczypię jemy pragnął zacność czytając ojca lasy Nowa wewnątrz klasztoru. Chce nóg mego wami. Zamku stał nogą imion ludzi ustaw Białopiotrowiczem. Kwiat Niesiołowskiemu nierostrzygniony Staje brał Nauka dachu dumę Zamku Kościuszkowskie zagon. Jakowaś zapytać dwie mój sama polu uszakach obyczaje Mój. Niesiołowski książkowéj zimny mały dotychczasowa Stryj przestraszone Stolnikównie wdał śmiertelnego. Stanisława charty kapeluszach mięty bratem każda brząknął rydwan.",
          "Мелких против летают хижину тмится. Чудесам возьмет звездна Взжигай. . Податель сельские мучитель сверкает очищаясь пламенем. Увы имя меч Мое сия. Устранюсь воздушных Им от До мысленные потушатся Ко Ея терпеньем.",
          "dotyku. Výdech spalin bude položen záplavový detekční kabely 1x UPS Newave Conceptpower DPA 5x 40kVA bude ukončen v samostatné strojovně. Samotné servery mají pouze lokalita Ústí nad zdvojenou podlahou budou zakončené GateWayí HiroLink - Monitoring rozvaděče RTN na jednotlivých záplavových zón na soustrojí resp. technologie jsou označeny SA-MKx.y. Jejich výstupem je zajištěn přestupem dat z jejich provoz. Na dveřích vylepené výstražné tabulky. Kabeláž z okruhů zálohovaných obvodů v R.MON-I. Monitoring EZS, EPS, ... možno zajistit funkčností FireWallů na strukturovanou kabeláží vedenou v měrných jímkách zapuštěných v každém racku budou zakončeny v R.MON-NrNN. Monitoring motorgenerátorů: řídící systém bude zakončena v modulu",
          "ramien mu zrejme vôbec niekto je už presne čo mám tendenciu prispôsobiť dych jej páčil, čo chce. Hmm... Včera sa mi pozdava, len dočkali, ale keďže som na uz boli u jej nezavrela. Hlava jej to ve městě nepotká, hodně mi to tí vedci pri hre, keď je tu pre Designiu. Pokiaľ viete o odbornejšie texty. Prvým z tmavých uličiek, každý to niekedy, zrovnávať krok s obrovským batohom na okraj vane a temné úmysly, tak rozmýšľam, aký som si hromady mailov, čo chcem a neraz sa pokúšal o filmovém klubu v budúcnosti rozhodne uniesť mladú maliarku (Linda Rybová), ktorú so",
          " 復讐者」. 復讐者」. 伯母さん 復讐者」. 復讐者」. 復讐者」. 復讐者」. 第九章 第五章 第六章 第七章 第八章. 復讐者」 伯母さん. 復讐者」 伯母さん. 第十一章 第十九章 第十四章 第十八章 第十三章 第十五章. 復讐者」 . 第十四章 第十一章 第十二章 第十五章 第十七章 手配書. 第十四章 手配書 第十八章 第十七章 第十六章 第十三章. 第十一章 第十三章 第十八章 第十四章 手配書. 復讐者」."
        ];

        texts.forEach(function (t, i) {
          var doc = c.document("text" + i);

          assertEqual(t, doc.value);
        });

      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test committed trx
      ////////////////////////////////////////////////////////////////////////////////

      testTransactionCommit: function () {
        var c = getCollection("UnitTestsDumpTransactionCommit");

        assertEqual(1000, c.count());

        for (var i = 0; i < 1000; ++i) {
          var doc = c.document("test" + i);

          assertEqual(i, doc.value1);
          assertEqual("this is a test", doc.value2);
          assertEqual("test" + i, doc.value3);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test committed trx
      ////////////////////////////////////////////////////////////////////////////////

      testTransactionUpdate: function () {
        var c = getCollection("UnitTestsDumpTransactionUpdate");

        assertEqual(1000, c.count());

        for (var i = 0; i < 1000; ++i) {
          var doc = c.document("test" + i);

          assertEqual(i, doc.value1);
          assertEqual("this is a test", doc.value2);
          if (i % 2 === 0) {
            assertEqual(i, doc.value3);
          } else {
            assertEqual("test" + i, doc.value3);
          }
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test aborted trx
      ////////////////////////////////////////////////////////////////////////////////

      testTransactionAbort: function () {
        var c = getCollection("UnitTestsDumpTransactionAbort");

        assertEqual(1, c.count());

        assertTrue(c.exists("foo"));
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test persistent
      ////////////////////////////////////////////////////////////////////////////////

      testPersistent: function () {
        var c = getCollection("UnitTestsDumpPersistent");
        var p = c.properties();

        assertEqual(2, c.indexes().length, `We found following indexes: ${JSON.stringify(c.indexes())}`);
        assertEqual("primary", c.indexes()[0].type);
        assertEqual("persistent", c.indexes()[1].type);
        assertEqual(10000, c.count());

        var res = db._query("FOR doc IN " + c.name() + " FILTER doc.value >= 0 RETURN doc").toArray();
        assertEqual(10000, res.length);

        res = db._query("FOR doc IN " + c.name() + " FILTER doc.value >= 5000 RETURN doc").toArray();
        assertEqual(5000, res.length);

        res = db._query("FOR doc IN " + c.name() + " FILTER doc.value >= 9000 RETURN doc").toArray();
        assertEqual(1000, res.length);

        res = db._query("FOR doc IN " + c.name() + " FILTER doc.value >= 10000 RETURN doc").toArray();
        assertEqual(0, res.length);
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test view restoring
      ////////////////////////////////////////////////////////////////////////////////

      testView: function () {
        try {
          db._createView("check", "arangosearch", {});
        } catch (err) {
        }

        let views = db._views();
        if (views.length === 0) {
          return; // arangosearch views are not supported
        }

        let view = db._view("UnitTestsDumpView");
        assertNotEqual(null, view);
        let props = view.properties();
        assertEqual("UnitTestsDumpView", view.name());
        assertEqual(Object.keys(props.links).length, 1);
        assertTrue(props.hasOwnProperty("links"));
        assertTrue(props.links.hasOwnProperty("UnitTestsDumpViewCollection"));
        assertTrue(props.links.UnitTestsDumpViewCollection.hasOwnProperty("includeAllFields"));
        assertTrue(props.links.UnitTestsDumpViewCollection.hasOwnProperty("fields"));
        assertTrue(props.links.UnitTestsDumpViewCollection.includeAllFields);
        assertEqual(Object.keys(props.links.UnitTestsDumpViewCollection.fields).length, 1);
        assertTrue(props.links.UnitTestsDumpViewCollection.fields.text.analyzers.length, 2);
        assertTrue("text_en", props.links.UnitTestsDumpViewCollection.fields.text.analyzers[0]);
        assertTrue("UnitTestsDumpView::custom", props.links.UnitTestsDumpViewCollection.fields.text.analyzers[1]);

        assertEqual(props.consolidationIntervalMsec, 0);
        assertEqual(props.cleanupIntervalStep, 456);
        assertTrue(Math.abs(props.consolidationPolicy.threshold - 0.3) < 0.001);
        assertEqual(props.consolidationPolicy.type, "bytes_accum");

        // After a restore, the ArangoSearch index needs to be rebuilt, therefore
        // we cannot expect that the data is immediately visible. We can only
        // hope that the data will be there in due course:

        let res = db._query("FOR doc IN " + view.name() + " SEARCH doc.value >= 0 OPTIONS { waitForSync: true } RETURN doc").toArray();
        assertEqual(5000, res.length);

        res = db._query("FOR doc IN " + view.name() + " SEARCH doc.value >= 2500 RETURN doc").toArray();
        assertEqual(2500, res.length);

        res = db._query("FOR doc IN " + view.name() + " SEARCH doc.value >= 5000 RETURN doc").toArray();
        assertEqual(0, res.length);

        res = db._query("FOR doc IN UnitTestsDumpView SEARCH PHRASE(doc.text, 'foxx jumps over', 'text_en') RETURN doc").toArray();
        assertEqual(1, res.length);
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test search-alias restoring
      ////////////////////////////////////////////////////////////////////////////////

      testSearch: function () {
        let collection = getCollection("searchCollection");
        assertNotEqual(null, collection);

        let index = collection.index("searchIndex");
        assertTrue(index.includeAllFields);
        assertEqual(index.consolidationIntervalMsec, 0);
        assertEqual(index.cleanupIntervalStep, 42);

        let search = db._view("searchSearch");
        assertNotEqual(null, search);
        assertEqual("searchSearch", search.name());
        let props = search.properties();
        assertEqual(props.indexes, [{ collection: "searchCollection", index: "searchIndex" }]);

        // After a restore, the Inverted index needs to be rebuilt, therefore
        // we cannot expect that the data is immediately visible. We can only
        // hope that the data will be there in due course:

        let res = db._query("FOR doc IN " + search.name() + " SEARCH doc.value >= 0 OPTIONS { waitForSync: true } RETURN doc").toArray();
        assertEqual(5000, res.length);

        if (isServer) {
          request({
            method: "get",
            url: "/_db/_system/_admin/metrics?mode=trigger_global",
            headers: {accept: "application/json"},
            body: {}
          });
        } else {
          let coordinators = getEndpointsByType("coordinator");
          for (let i = 0; i < coordinators.length; i++) {
            let c = coordinators[i];
            getRawMetric(c, '?mode=trigger_global');
          }
        }
        let figures;
        for (let i = 0; i < 100; ++i) {
          require("internal").sleep(0.5);
          figures = collection.indexes(true, true)
            .find(e => e.name === "searchIndex")
            .figures;
          if (figures.numDocs > 5000) {
            break;
          }
        }
        assertEqual(figures.numDocs, 5001);
        assertEqual(figures.numLiveDocs, 5001);
        assertEqual(figures.numPrimaryDocs, 5001);
        assertTrue(figures.numSegments >= 1);
        assertTrue(figures.numFiles >= 7);
        assertTrue(figures.indexSize > 0);

        res = db._query("FOR doc IN " + search.name() + " SEARCH doc.value >= 2500 RETURN doc").toArray();
        assertEqual(2500, res.length);

        res = db._query("FOR doc IN " + search.name() + " SEARCH doc.value >= 5000 RETURN doc").toArray();
        assertEqual(0, res.length);

        res = db._query("FOR doc IN " + search.name() + " SEARCH PHRASE(doc.text, 'foxx jumps over', 'text_en') RETURN doc").toArray();
        assertEqual(1, res.length);
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test inverted index restoring
      ////////////////////////////////////////////////////////////////////////////////

      testInvertedIndex: function () {
        let collection = getCollection("collection_documents_0");
        assertNotEqual(null, collection);

        let index = collection.index("collection_documents_0_all");

        let search = db._view("v_collection_documents_0_all");
        assertNotEqual(null, search);
        assertEqual("v_collection_documents_0_all", search.name());
        let props = search.properties();
        assertEqual(props.indexes, [ { 'collection': 'collection_documents_0', 'index': 'collection_documents_0_all' }]);
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test custom analyzers restoring
      ////////////////////////////////////////////////////////////////////////////////
      testAnalyzers: function () {
        /*
         * createAnalyzers() creates -> "customInCreateAnalyzers" (CE && EE)
         * createSmartArangoSearch() creates -> "smartCustom" (EE)
         * createView() creates -> customInCreateView (CE && EE)
         *
         * => EE will lead to a total of 3x created analyzers
         * => CE will lead to a total of 2x created analyzers
         */
        let createdAnalyzersInSrc = [
          {
            name: 'customInCreateView',
            delimiter: " "
          }, {
            name: 'customInCreateAnalyzers',
            delimiter: ","
          }
        ];
        if (isEnterprise) {
          createdAnalyzersInSrc.push({
            name: "smartCustom",
            delimiter: "smart"
          });
        }

        let verifyAnalyzer = (analyzerName, delimiter) => {
          let analyzer = analyzers.analyzer(analyzerName);
          assertEqual(db._name() + "::" + analyzerName, analyzer.name());
          assertEqual("delimiter", analyzer.type());
          assertEqual(Object.keys(analyzer.properties()).length, 1);
          assertEqual(delimiter, analyzer.properties().delimiter);
          assertEqual(1, analyzer.features().length);
          assertEqual("frequency", analyzer.features()[0]);
        };

        assertNotEqual(null, getCollection("_analyzers"));
        assertEqual((isEnterprise) ?
            7 : // plus 1 for a SmartGraph in Enterprise
            6,  // only 1 stored custom analyzers
          db._analyzers.count());

        _.each(createdAnalyzersInSrc, (analyzerObject) => {
          verifyAnalyzer(analyzerObject.name, analyzerObject.delimiter);
        });

        assertNull(analyzers.analyzer("custom_dst"));
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test link on analyzers collection
      ////////////////////////////////////////////////////////////////////////////////
      testIndexAnalyzerCollection: function () {
        let res = db._query("FOR d IN analyzersView OPTIONS {waitForSync:true} FOR a IN _analyzers FILTER d._key == a._key RETURN [d,a]").toArray();
        assertEqual(res.length, db._analyzers.count());
        res.forEach(function (e) {
          assertEqual(e[0], e[1]);
        });
      },

      testJobsAndQueues: function () {
        assertEqual("test", db._jobs.document("test")._key);
        assertEqual("test", db._queues.document("test")._key);
      },

      testReplicationFactor: function () {
        let c = getCollection("UnitTestsDumpReplicationFactor1");
        let p = c.properties();

        assertEqual(2, p.replicationFactor);
        assertEqual(getNoShards(7), p.numberOfShards);

        c = getCollection("UnitTestsDumpReplicationFactor2");
        p = c.properties();

        assertEqual(2, p.replicationFactor);
        assertEqual(getNoShards(6), p.numberOfShards);
      },

      testSatelliteGraph: function () {
        {
          // check collections in graph object
          const satelliteGraph = satgm._graph(satelliteGraphName);
          const actualVertexCollections = Object.keys(satelliteGraph._vertexCollections(true)).sort();
          const expectedVertexCollections = [satelliteVertexCollection1Name, satelliteVertexCollection2Name].sort();
          assertEqual(expectedVertexCollections, actualVertexCollections);
          const expectedOrphanCollections = [satelliteOrphanCollectionName].sort();
          const actualOrphanCollections = satelliteGraph._orphanCollections().sort();
          assertEqual(expectedOrphanCollections, actualOrphanCollections);
          const expectedEdgeCollections = [satelliteEdgeCollection1Name, satelliteEdgeCollection2Name].sort();
          const actualEdgeCollections = satelliteGraph._edgeCollections().map(c => c.name()).sort();
          assertEqual(expectedEdgeCollections, actualEdgeCollections);
        }

        {
          // check edge definitions
          const graphInfo = arangosh.checkRequestResult(db._connection.GET('/_api/gharial/' + satelliteGraphName));
          assertTypeOf('object', graphInfo);
          assertTypeOf('object', graphInfo.graph);
          assertTypeOf('object', graphInfo.graph.edgeDefinitions);
          graphInfo.graph.edgeDefinitions.sort();
          const expectedEdgeDefinitions = [
            {
              collection: satelliteEdgeCollection1Name,
              from: [satelliteVertexCollection1Name],
              to: [satelliteVertexCollection2Name],
            },
            {
              collection: satelliteEdgeCollection2Name,
              from: [satelliteVertexCollection2Name],
              to: [satelliteVertexCollection1Name],
            },
          ].sort();
          assertEqual(expectedEdgeDefinitions, graphInfo.graph.edgeDefinitions);
        }

        {
          // check replicationFactor and distributeShardsLike of collections
          assertEqual("satellite", db[satelliteVertexCollection1Name].properties().replicationFactor);
          assertEqual("satellite", db[satelliteVertexCollection2Name].properties().replicationFactor);
          assertEqual("satellite", db[satelliteEdgeCollection1Name].properties().replicationFactor);
          assertEqual("satellite", db[satelliteEdgeCollection2Name].properties().replicationFactor);
          assertEqual("satellite", db[satelliteOrphanCollectionName].properties().replicationFactor);
          assertUndefined(db[satelliteVertexCollection1Name].properties().distributeShardsLike);
          assertEqual(satelliteVertexCollection1Name, db[satelliteVertexCollection2Name].properties().distributeShardsLike);
          assertEqual(satelliteVertexCollection1Name, db[satelliteEdgeCollection1Name].properties().distributeShardsLike);
          assertEqual(satelliteVertexCollection1Name, db[satelliteEdgeCollection2Name].properties().distributeShardsLike);
          assertEqual(satelliteVertexCollection1Name, db[satelliteOrphanCollectionName].properties().distributeShardsLike);
        }

        {
          // check contents of vertex collections
          const getVertices = `FOR d IN @@col SORT TO_NUMBER(SUBSTRING(d._key, 1)) RETURN d._key`;
          // [ "v1", "v3", "v5", ..., "v99" ]
          const v1s = db._query(getVertices, {'@col': satelliteVertexCollection1Name}).toArray();
          const expectedV1s = _.range(1, 100 + 1, 2).map(i => `v${i}`);
          assertEqual(expectedV1s, v1s);
          // [ "v2", "v4", "v6", ..., "v100" ]
          const v2s = db._query(getVertices, {'@col': satelliteVertexCollection2Name}).toArray();
          const expectedV2s = _.range(2, 100 + 1, 2).map(i => `v${i}`);
          assertEqual(expectedV2s, v2s);
          // [ "w1", "w2", "w3", ..., "w100" ]
          const os = db._query(getVertices, {'@col': satelliteOrphanCollectionName}).toArray();
          const expectedOs = _.range(1, 100 + 1, 1).map(i => `w${i}`);
          assertEqual(expectedOs, os);

          // check contents of edge collections
          const getEdges = `
          FOR e IN @@col
            LET from = PARSE_IDENTIFIER(e._from).key
            LET to = PARSE_IDENTIFIER(e._to).key
            SORT TO_NUMBER(SUBSTRING(from, 1)), TO_NUMBER(SUBSTRING(to, 1))
            RETURN [from, to]
        `;
          // [ "v1" -> "v2", "v3" -> "v4", "v5" -> "v6", ..., "v99" -> "v100" ]
          const e1s = db._query(getEdges, {'@col': satelliteEdgeCollection1Name}).toArray();
          const expectedE1s = _.range(1, 100 + 1, 2).map(i => [`v${i}`, `v${i % 100 + 1}`]);
          assertEqual(expectedE1s, e1s);
          // [ "v2" -> "v3", "v4" -> "v5", "v6" -> "v7", ..., "v98" -> "v99", "v100" -> "v1" ]
          const e2s = db._query(getEdges, {'@col': satelliteEdgeCollection2Name}).toArray();
          const expectedE2s = _.range(2, 100 + 1, 2).map(i => [`v${i}`, `v${i % 100 + 1}`]);
          assertEqual(expectedE2s, e2s);
        }

        {
          // run a simple traversal
          const res = db._query(`
          FOR v, e, p IN 100 OUTBOUND "UnitTestDumpSatelliteVertexCollection1/v1" GRAPH "UnitTestDumpSatelliteGraph"
            RETURN p.vertices[*]._key
        `);
          // [ [ "v1", "v2", ..., "v99", "v100", "v1" ] ]
          const expected = [_.range(0, 100 + 1).map(i => "v" + (i % 100 + 1))];
          assertEqual(expected, res.toArray());
        }
      },

      testHiddenCollectionsOmitted: function () {
        const dumpDir = getDumpDir();
        const smartEdgeCollectionPath = fs.join(dumpDir, `${edges}.structure.json`);
        const localEdgeCollectionPath = fs.join(dumpDir, `_local_${edges}.structure.json`);
        const fromEdgeCollectionPath = fs.join(dumpDir, `_from_${edges}.structure.json`);
        const toEdgeCollectionPath = fs.join(dumpDir, `_to_${edges}.structure.json`);

        assertTrue(fs.exists(smartEdgeCollectionPath), 'Smart edge collection missing in dump!');
        assertFalse(fs.exists(localEdgeCollectionPath), '_local edge collection should not have been dumped!');
        assertFalse(fs.exists(fromEdgeCollectionPath), '_from edge collection should not have been dumped!');
        assertFalse(fs.exists(toEdgeCollectionPath), '_to edge collection should not have been dumped!');
      },

      testShadowCollectionsOmitted: function () {
        const dumpDir = getDumpDir();
        const encryption = fs.read(fs.join(dumpDir, 'ENCRYPTION'));
        if (encryption === '' || encryption === 'none') {
          const collStructure = JSON.parse(
            fs.read(fs.join(dumpDir, `${edges}.structure.json`))
          );

          assertTrue(collStructure.hasOwnProperty('parameters'), collStructure);
          const parameters = collStructure['parameters'];
          assertFalse(parameters.hasOwnProperty('shadowCollections'),
            `Property 'shadowCollections' should be hidden in collection ${edges}!`);
        }
      },

      testVertices: function () {
        let c = getCollection(vertices);
        let p = c.properties();
        assertEqual(2, c.type()); // Document
        assertEqual(getNoShards(5), p.numberOfShards);
        assertTrue(p.isSmart, p);
        assertEqual(100, c.count());
        assertEqual("value", p.smartGraphAttribute);
      },

      testVerticesAqlRead: function () {
        let q1 = `FOR x IN ${vertices} SORT TO_NUMBER(x.value) RETURN x`;
        let q2 = `FOR x IN ${vertices} FILTER x.value == "10" RETURN x.value`;
        // This query can be optimized to a single shard. Make sure that is still correct
        let q3 = `FOR x IN ${vertices} FILTER x._key == @key RETURN x.value`;

        let res1 = db._query(q1).toArray();
        assertEqual(100, res1.length);
        for (let i = 0; i < 100; ++i) {
          assertEqual(String(i), res1[i].value);
        }

        let res2 = db._query(q2).toArray();
        assertEqual(1, res2.length);
        assertEqual("10", res2[0]);

        for (let x of res1) {
          let res3 = db._query(q3, {key: x._key}).toArray();
          assertEqual(1, res3.length);
          assertEqual(x.value, res3[0]);
        }
      },

      testVerticesAqlInsert: function () {
        // Precondition
        assertEqual(100, db[vertices].count());
        let insert = `FOR i IN 0..99 INSERT {value: TO_STRING(i), needUpdate: true, needRemove: true} INTO ${vertices}`;
        let update = `FOR x IN ${vertices} FILTER x.needUpdate UPDATE x WITH {needUpdate: false} INTO ${vertices}`;
        let remove = `FOR x IN ${vertices} FILTER x.needRemove REMOVE x INTO ${vertices}`;
        // Note: Order is important here, we first insert, than update those inserted docs, then remoe them again
        let resIns = db._query(insert);
        assertEqual(100, resIns.getExtra().stats.writesExecuted);
        assertEqual(0, resIns.getExtra().stats.writesIgnored);
        assertEqual(200, db[vertices].count());

        let resUp = db._query(update);
        assertEqual(100, resUp.getExtra().stats.writesExecuted);
        assertEqual(0, resUp.getExtra().stats.writesIgnored);
        assertEqual(200, db[vertices].count());

        let resRem = db._query(remove);
        assertEqual(100, resRem.getExtra().stats.writesExecuted);
        assertEqual(0, resRem.getExtra().stats.writesIgnored);
        assertEqual(100, db[vertices].count());
      },

      testOrphans: function () {
        let c = getCollection(orphans);
        let p = c.properties();
        assertEqual(2, c.type()); // Document
        assertEqual(getNoShards(5), p.numberOfShards);
        assertTrue(p.isSmart);
        assertEqual(100, c.count());
        assertEqual("value", p.smartGraphAttribute);
      },

      testOrphansAqlRead: function () {
        let q1 = `FOR x IN ${orphans} SORT TO_NUMBER(x.value) RETURN x`;
        let q2 = `FOR x IN ${orphans} FILTER x.value == "10" RETURN x.value`;
        // This query can be optimized to a single shard. Make sure that is still correct
        let q3 = `FOR x IN ${orphans} FILTER x._key == @key RETURN x.value`;

        let res1 = db._query(q1).toArray();
        assertEqual(100, res1.length);
        for (let i = 0; i < 100; ++i) {
          assertEqual(String(i), res1[i].value);
        }

        let res2 = db._query(q2).toArray();
        assertEqual(1, res2.length);
        assertEqual("10", res2[0]);

        for (let x of res1) {
          let res3 = db._query(q3, {key: x._key}).toArray();
          assertEqual(1, res3.length);
          assertEqual(x.value, res3[0]);
        }
      },

      testOrphansAqlInsert: function () {
        // Precondition
        let c = db[orphans];
        assertEqual(100, c.count());
        let insert = `FOR i IN 0..99 INSERT {value: TO_STRING(i), needUpdate: true, needRemove: true} INTO ${orphans}`;
        let update = `FOR x IN ${orphans} FILTER x.needUpdate UPDATE x WITH {needUpdate: false} INTO ${orphans}`;
        let remove = `FOR x IN ${orphans} FILTER x.needRemove REMOVE x INTO ${orphans}`;
        // Note: Order is important here, we first insert, than update those inserted docs, then remoe them again
        let resIns = db._query(insert);
        assertEqual(100, resIns.getExtra().stats.writesExecuted);
        assertEqual(0, resIns.getExtra().stats.writesIgnored);
        assertEqual(200, c.count());

        let resUp = db._query(update);
        assertEqual(100, resUp.getExtra().stats.writesExecuted);
        assertEqual(0, resUp.getExtra().stats.writesIgnored);
        assertEqual(200, c.count());

        let resRem = db._query(remove);
        assertEqual(100, resRem.getExtra().stats.writesExecuted);
        assertEqual(0, resRem.getExtra().stats.writesIgnored);
        assertEqual(100, c.count());
      },

      testEEEdges: function () {
        let c = getCollection(edges);
        let p = c.properties();
        assertEqual(3, c.type()); // Edges
        //assertEqual(getNoShards(5), p.numberOfShards);
        assertTrue(p.isSmart);
        assertEqual(300, c.count());
      },

      testEdgesAqlRead: function () {
        let q1 = `FOR x IN ${edges} SORT TO_NUMBER(x.value) RETURN x`;
        let q2 = `FOR x IN ${edges} FILTER x.value == "10" RETURN x.value`;
        // This query can be optimized to a single shard. Make sure that is still correct
        let q3 = `FOR x IN ${edges} FILTER x._key == @key RETURN x.value`;

        let res1 = db._query(q1).toArray();
        assertEqual(300, res1.length);
        for (let i = 0; i < 100; ++i) {
          // We have three edges per value
          assertEqual(String(i), res1[3 * i].value);
          assertEqual(String(i), res1[3 * i + 1].value);
          assertEqual(String(i), res1[3 * i + 2].value);
        }

        let res2 = db._query(q2).toArray();
        assertEqual(3, res2.length);
        assertEqual("10", res2[0]);

        for (let x of res1) {
          let res3 = db._query(q3, {key: x._key}).toArray();
          assertEqual(1, res3.length);
          assertEqual(x.value, res3[0]);
        }
      },

      testEdgesAqlInsert: function () {
        // Precondition
        let c = db[edges];
        assertEqual(300, c.count());

        // We first need the vertices
        let vC = db[vertices];
        assertEqual(100, vC.count());
        let vQ = `FOR x IN ${vertices} SORT TO_NUMBER(x.value) RETURN x._id`;
        let verticesList = db._query(vQ).toArray();
        let insertSameValue = `LET vs = @vertices FOR i IN 0..99 INSERT {_from: vs[i], _to: vs[i], value: TO_STRING(i), needUpdate: true, needRemove: true} INTO ${edges}`;
        let insertOtherValue = `LET vs = @vertices FOR i IN 0..99 INSERT {_from: vs[i], _to: vs[(i + 1) % 100], value: TO_STRING(i), needUpdate: true, needRemove: true} INTO ${edges}`;
        let update = `FOR x IN ${edges} FILTER x.needUpdate UPDATE x WITH {needUpdate: false} INTO ${edges}`;
        let remove = `FOR x IN ${edges} FILTER x.needRemove REMOVE x INTO ${edges}`;
        // Note: Order is important here, we first insert, than update those inserted docs, then remove them again
        let resInsSame = db._query(insertSameValue, {vertices: verticesList});
        assertEqual(100, resInsSame.getExtra().stats.writesExecuted);
        assertEqual(0, resInsSame.getExtra().stats.writesIgnored);
        assertEqual(400, c.count());

        let resInsOther = db._query(insertOtherValue, {vertices: verticesList});
        assertEqual(100, resInsOther.getExtra().stats.writesExecuted);
        assertEqual(0, resInsOther.getExtra().stats.writesIgnored);
        assertEqual(500, c.count());

        let resUp = db._query(update);
        assertEqual(200, resUp.getExtra().stats.writesExecuted);
        assertEqual(0, resUp.getExtra().stats.writesIgnored);
        assertEqual(500, c.count());

        let resRem = db._query(remove);
        assertEqual(200, resRem.getExtra().stats.writesExecuted);
        assertEqual(0, resRem.getExtra().stats.writesIgnored);
        assertEqual(300, c.count());
      },

      testAqlGraphQueryOutbound: function () {
        // Precondition
        const c = db[edges];
        assertEqual(300, c.count());
        // We first need the vertices
        const vC = db[vertices];
        assertEqual(100, vC.count());

        const vertexQuery = `FOR x IN ${vertices} FILTER x.value == "10" RETURN x._id`;
        const vertex = db._query(vertexQuery).toArray();
        assertEqual(1, vertex.length);

        const q = `FOR v IN 1..2 OUTBOUND "${vertex[0]}" GRAPH "${smartGraphName}" OPTIONS {uniqueVertices: 'path'}
        SORT TO_NUMBER(v.value) RETURN v`;
        /* We expect the following paths:
         * 10 -> 9 -> 8
         * 10 <- 9
         * 10 -> 11
         * 10 -> 11 -> 12
         */

        //Validate that everything is wired to a SmartGraph correctly
        const res = db._query(q).toArray();
        assertEqual(4, res.length);
        assertEqual("8", res[0].value);
        assertEqual("9", res[1].value);
        assertEqual("11", res[2].value);
        assertEqual("12", res[3].value);
      },

      testAqlGraphQueryAny: function () {
        // Precondition
        const c = db[edges];
        assertEqual(300, c.count());
        // We first need the vertices
        const vC = db[vertices];
        assertEqual(100, vC.count());

        const vertexQuery = `FOR x IN ${vertices} FILTER x.value == "10" RETURN x._id`;
        const vertex = db._query(vertexQuery).toArray();
        assertEqual(1, vertex.length);

        const q = `FOR v IN 1..2 ANY "${vertex[0]}" GRAPH "${smartGraphName}" OPTIONS {uniqueVertices: 'path'}
        SORT TO_NUMBER(v.value) RETURN v.value`;
        /* We expect the following paths:
         * 10 -> 9 -> 8
         * 10 -> 9 <- 8
         * 10 <- 9 -> 8
         * 10 <- 9 <- 8
         * 10 <- 9
         * 10 -> 9
         * 10 -> 11
         * 10 <- 11
         * 10 -> 11 -> 12
         * 10 -> 11 <- 12
         * 10 <- 11 -> 12
         * 10 <- 11 <- 12
         */

        //Validate that everything is wired to a SmartGraph correctly
        const res = db._query(q).toArray();
        assertEqual('8 8 8 8 9 9 11 11 12 12 12 12'.split(' '), res);
      },

      testSmartGraphSharding: function () {
        if (db._properties().sharding === "single") {
          print("Skipping testSmartGraphSharding for oneshard db");
          return 0;
        }
        const eCol = getCollection(edges);
        const eProp = eCol.properties();
        const vCol = getCollection(vertices);
        const vProp = vCol.properties();
        const oCol = getCollection(orphans);
        const oProp = oCol.properties();
        // It is random if a vertex collection or an orphan
        // collection is selected to lead the shard distribution.
        // But one of the two has to be selected, edges is
        // impossible
        if (oProp.hasOwnProperty("distributeShardsLike")) {
          // The Vertex collection is selected as leading.
          assertFalse(Object.hasOwnProperty(vProp, "distributeShardsLike"));
          assertEqual(vertices, eProp.distributeShardsLike, `Not equal: ${JSON.stringify(vertices)} !~= ${JSON.stringify(eProp)}`);
          assertEqual(vertices, oProp.distributeShardsLike, `Not equal: ${JSON.stringify(vertices)} !~= ${JSON.stringify(eProp)}`);
        } else {
          // The orphan collection is selected as leading.
          assertFalse(Object.hasOwnProperty(oProp, "distributeShardsLike"));
          assertEqual(orphans, eProp.distributeShardsLike);
          assertEqual(orphans, vProp.distributeShardsLike);
        }
      },

      testViewOnSmartEdgeCollection: function () {
        try {
          db._createView("check", "arangosearch", {});
        } catch (err) {
        }

        let views = db._views();
        if (views.length === 0) {
          return; // arangosearch views are not supported
        }

        let view = db._view("UnitTestsDumpSmartView");
        assertNotEqual(null, view);
        let props = view.properties();
        assertEqual("UnitTestsDumpSmartView", view.name());
        assertTrue(props.hasOwnProperty("links"));
        assertEqual(Object.keys(props.links).length, 3); // virtual collecion + 2 system collections

        // UnitTestDumpSmartEdges
        assertTrue(props.links.hasOwnProperty("UnitTestDumpSmartEdges"));
        assertTrue(props.links.UnitTestDumpSmartEdges.hasOwnProperty("includeAllFields"));
        assertTrue(props.links.UnitTestDumpSmartEdges.includeAllFields);
        assertTrue(props.links.UnitTestDumpSmartEdges.hasOwnProperty("fields"));
        assertEqual(Object.keys(props.links.UnitTestDumpSmartEdges.fields).length, 1);
        assertTrue(props.links.UnitTestDumpSmartEdges.fields.text.analyzers.length, 2);
        assertTrue("text_en", props.links.UnitTestDumpSmartEdges.fields.text.analyzers[0]);
        assertTrue("UnitTestsDumpView::smartCustom", props.links.UnitTestDumpSmartEdges.fields.text.analyzers[1]);

        // _to_UnitTestDumpSmartEdges
        assertFalse(props.links.hasOwnProperty("_to_UnitTestDumpSmartEdges"));

        // _from_UnitTestDumpSmartEdges
        assertTrue(props.links.hasOwnProperty("_from_UnitTestDumpSmartEdges"));
        assertTrue(props.links._from_UnitTestDumpSmartEdges.hasOwnProperty("includeAllFields"));
        assertTrue(props.links._from_UnitTestDumpSmartEdges.includeAllFields);
        assertTrue(props.links._from_UnitTestDumpSmartEdges.hasOwnProperty("fields"));
        assertEqual(Object.keys(props.links._from_UnitTestDumpSmartEdges.fields).length, 1);
        assertTrue(props.links._from_UnitTestDumpSmartEdges.fields.text.analyzers.length, 2);
        assertTrue("text_en", props.links._from_UnitTestDumpSmartEdges.fields.text.analyzers[0]);
        assertTrue("UnitTestsDumpView::smartCustom", props.links._from_UnitTestDumpSmartEdges.fields.text.analyzers[1]);

        // _local_UnitTestDumpSmartEdges
        assertTrue(props.links.hasOwnProperty("_local_UnitTestDumpSmartEdges"));
        assertTrue(props.links._local_UnitTestDumpSmartEdges.hasOwnProperty("includeAllFields"));
        assertTrue(props.links._local_UnitTestDumpSmartEdges.includeAllFields);
        assertTrue(props.links._local_UnitTestDumpSmartEdges.hasOwnProperty("fields"));
        assertEqual(Object.keys(props.links._local_UnitTestDumpSmartEdges.fields).length, 1);
        assertTrue(props.links._local_UnitTestDumpSmartEdges.fields.text.analyzers.length, 2);
        assertTrue("text_en", props.links._local_UnitTestDumpSmartEdges.fields.text.analyzers[0]);
        assertTrue("UnitTestsDumpView::smartCustom", props.links._local_UnitTestDumpSmartEdges.fields.text.analyzers[1]);
        assertEqual(props.consolidationIntervalMsec, 0);
        assertEqual(props.cleanupIntervalStep, 456);
        assertTrue(Math.abs(props.consolidationPolicy.threshold - 0.3) < 0.001);
        assertEqual(props.consolidationPolicy.type, "bytes_accum");
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test whether hotbackup restore restores the uniqid
      ////////////////////////////////////////////////////////////////////////////////
      testLatestId: function () {
        if (arango.getRole() === "COORDINATOR") {
          // Only executed in the cluster
          // After the hotbackup was taken, we have consumed 100.000.000
          // cluster wide unique ids. Here, we check that after the
          // hotbackup restore the number in /arango/Sync/LatestID
          // in the agency is lower again.
          let next = JSON.parse(db._connection.POST("/_admin/execute?returnAsJSON=true", "return global.ArangoAgency.uniqid(1)"));
          assertTrue(next < 100000000, "expected next uniqid after restore to be less than 100000000, not " + next);
        }
      },

      ////////////////////////////////////////////////////////////////////////////////
      /// @brief test smart graph restores
      ////////////////////////////////////////////////////////////////////////////////
      testEmptySmartGraph: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestsDumpEmptySmartGraph";
        const graphsColl = getCollection('_graphs');
        assertTrue(!!graphsColl.exists(smartGraphName));

        const graph = graphsColl.document(smartGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonSmartGraphProperties(graph);
        assertEqual(graph.edgeDefinitions.length, 0);
        assertEqual(graph.orphanCollections.length, 0);
      },

      testSmartGraph3_11_compat: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestDumpSmartGraphCompat";
        const graphsColl = getCollection('_graphs');
        assertTrue(!!graphsColl.exists(smartGraphName));

        const graphDoc = graphsColl.document(smartGraphName);
        // This is the requirement to test if the compat cid has effect
        // We test this here as the setup for this graph should fake an
        // old version graph, so we can test that the values existence
        // does not have negative impact on the restore.
        assertTrue(graphDoc.hasOwnProperty('initialCid'));
        // Now let us try to modify the GraphRelations
        const newRelationName = `${smartGraphName}NewEdge`;
        const newVertexName = `${smartGraphName}NewVertex`;
        const newRelation = gm._relation(newRelationName,
          "UnitTestDumpSmartVerticesCompat",
          newVertexName
        );
        // New collections do not yet exist, that would be forbidden.
        assertEqual(db._collections().filter(c => c.name() === newRelationName).length, 0);
        assertEqual(db._collections().filter(c => c.name() === newVertexName).length, 0);
        const graph = gm._graph(smartGraphName);
        graph._extendEdgeDefinitions(newRelation);
        // New collections are added
        assertEqual(db._collections().filter(c => c.name() === newRelationName).length, 1);
        assertEqual(db._collections().filter(c => c.name() === newVertexName).length, 1);
        // New collections are smart
        assertTrue(db[newRelationName].properties().isSmart);
        assertTrue(db[newVertexName].properties().isSmart);
      },

      testEmptyEnterpriseGraph: function () {
        if (!isEnterprise) {
          return;
        }
        const enterpriseGraphName = "UnitTestsDumpEmptyEnterpriseGraph";
        const graphsColl = getCollection('_graphs');
        assertTrue(!!graphsColl.exists(enterpriseGraphName));

        const graph = graphsColl.document(enterpriseGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonEnterpriseGraphProperties(graph);
        assertEqual(graph.edgeDefinitions.length, 0);
        assertEqual(graph.orphanCollections.length, 0);
      },

      testEmptySatelliteGraph: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestsDumpEmptySatelliteGraph";
        const graphsColl = getCollection('_graphs');
        assertTrue(!!graphsColl.exists(smartGraphName));

        const graph = graphsColl.document(smartGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonSatelliteGraphProperties(graph);
        assertEmptyness(graph);
      },

      testEmptyDisjointGraph: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestsDumpEmptyDisjointGraph";
        const graphsColl = getCollection('_graphs');
        assertTrue(!!graphsColl.exists(smartGraphName));

        const graph = graphsColl.document(smartGraphName);
        assertTrue(graph.isDisjoint);
        assertCommonSmartGraphProperties(graph);
        assertEmptyness(graph);
      },

      testSmartGraphWithoutData: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestDumpSmartGraphWithoutData";
        const vertexCollName = 'UnitTestDumpSmartVerticesWithoutData';
        const edgeCollName = 'UnitTestDumpSmartEdgesWithoutData';
        const orphanCollName = 'UnitTestDumpSmartOrphansWithoutData';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(smartGraphName));

        const graph = g.document(smartGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonSmartGraphProperties(graph);
        assertSmartCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, false);
      },

      testEnterpriseGraphWithoutData: function () {
        if (!isEnterprise) {
          return;
        }
        const enterpriseGraphName = "UnitTestDumpEnterpriseGraphWithoutData";
        const vertexCollName = 'UnitTestDumpEnterpriseVerticesWithoutData';
        const edgeCollName = 'UnitTestDumpEnterpriseEdgesWithoutData';
        const orphanCollName = 'UnitTestDumpEnterpriseOrphansWithoutData';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(enterpriseGraphName));

        const graph = g.document(enterpriseGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonEnterpriseGraphProperties(graph);
        assertEnterpriseCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, false);
      },

      testSmartGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const smartGraphName = "UnitTestDumpSmartGraphSingleServer";
        const vertexCollName = 'UnitTestDumpSmartVerticesSingleServer';
        const edgeCollName = 'UnitTestDumpSmartEdgesSingleServer';
        const orphanCollName = 'UnitTestDumpSmartOrphansSingleServer';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(smartGraphName));

        const graph = g.document(smartGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonSmartGraphProperties(graph);
        assertSmartCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, false);

        const vColl = getCollection(vertexCollName);
        const eColl = getCollection(edgeCollName);
        const oColl = getCollection(orphanCollName);

        assertEqual(eColl.count(), 300);
        assertEqual(vColl.count(), 100);
        assertEqual(oColl.count(), 100);

        const getFromOrToSmartValue = (str) => {
          return str.substring(
            str.indexOf("/") + 1, str.lastIndexOf(":")
          );
        };

        for (const edge of eColl.toArray()) {
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);

          // Check that a well-formatted enterprise key exists (example: <fromSmart>:12345:<toSmart>);
          const fromSmartValue = getFromOrToSmartValue(edge._from);
          const toSmartValue = getFromOrToSmartValue(edge._to);
          const keySplit = edge._key.split(':');
          assertEqual(keySplit.length, 3);
          assertEqual(keySplit[0], fromSmartValue);
          assertEqual(keySplit[2], toSmartValue);
        }
      },

      testEnterpriseGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const enterpriseGraphName = "UnitTestDumpEnterpriseGraphSingleServer";
        const vertexCollName = 'UnitTestDumpEnterpriseVerticesSingleServer';
        const edgeCollName = 'UnitTestDumpEnterpriseEdgesSingleServer';
        const orphanCollName = 'UnitTestDumpEnterpriseOrphansSingleServer';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(enterpriseGraphName));

        const graph = g.document(enterpriseGraphName);
        assertFalse(graph.isDisjoint);
        assertFalse(graph.smartGraphAttribute);
        assertCommonEnterpriseGraphProperties(graph);
        assertEnterpriseCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, false);

        const vColl = getCollection(vertexCollName);
        const eColl = getCollection(edgeCollName);
        const oColl = getCollection(orphanCollName);

        assertEqual(eColl.count(), 300);
        assertEqual(vColl.count(), 100);
        assertEqual(oColl.count(), 100);

        for (const edge of eColl.toArray()) {
          assertTrue(!!edge._key);
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);

          // Check that a well-formatted enterprise key exists (example: abc:12345:xyz)
          const keySplit = edge._key.split(':');
          assertEqual(keySplit.length, 3);
          assertEqual(keySplit[0].length, 5);
          assertEqual(keySplit[2].length, 5);
        }
      },

      testSatelliteSmartGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const satelliteGraphName = "UnitTestDumpSatelliteGraphSingleServer";
        const vertexCollName = 'UnitTestDumpSatelliteVerticesSingleServer';
        const edgeCollName = 'UnitTestDumpSatelliteEdgesSingleServer';
        const orphanCollName = 'UnitTestDumpSatelliteOrphansSingleServer';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(satelliteGraphName));

        const graph = g.document(satelliteGraphName);
        assertFalse(graph.isDisjoint);
        assertCommonSatelliteGraphProperties(graph);
        assertSatelliteCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName);

        const vColl = getCollection(vertexCollName);
        const eColl = getCollection(edgeCollName);
        const oColl = getCollection(orphanCollName);

        assertEqual(eColl.count(), 300);
        assertEqual(vColl.count(), 100);
        assertEqual(oColl.count(), 100);

        for (const edge of eColl.toArray()) {
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);
        }
      },

      testDisjointGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const disjointGraphName = "UnitTestDumpDisjointGraphSingleServer";
        const vertexCollName = 'UnitTestDumpDisjointVerticesSingleServer';
        const edgeCollName = 'UnitTestDumpDisjointEdgesSingleServer';
        const orphanCollName = 'UnitTestDumpDisjointOrphansSingleServer';

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(disjointGraphName));

        const graph = g.document(disjointGraphName);
        assertCommonSmartGraphProperties(graph);
        assertSmartCollectionProperties(graph, edgeCollName, vertexCollName, orphanCollName, true);

        const vColl = getCollection(vertexCollName);
        const eColl = getCollection(edgeCollName);
        const oColl = getCollection(orphanCollName);

        // only self-loops are saved as the others have different values of the 'value' key, which is the smartGraphAttribute field
        assertEqual(eColl.count(), 100);
        assertEqual(vColl.count(), 100);
        assertEqual(oColl.count(), 100);

        for (const edge of eColl.toArray()) {
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);
        }

        // Additional test-case:
        // Add a non-disjoint edge to verify that this graph is properly created as a DisjointGraph
        // SmartGraphAttribute: "value"
        const firstVertex = vColl.save({value: "A"});
        const secondVertex = vColl.save({value: "A"});
        const thirdVertex = vColl.save({value: "B"});
        assertEqual(vColl.count(), 103);

        const firstEdge = eColl.save({_from: firstVertex._id, _to: secondVertex._id});
        assertEqual(eColl.count(), 101);

        try {
          // MUST fail
          eColl.save({_from: firstVertex._id, _to: thirdVertex._id});
          fail();
        } catch (err) {
          assertEqual(arangodb.ERROR_INVALID_DISJOINT_SMART_EDGE, err.errorNum);
        }

        assertEqual(eColl.count(), 101);
      },

      testHybridSmartGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const hybridGraphName = "UnitTestDumpHybridSmartGraphSingleServer";
        const verticesNonSat = "UnitTestDumpHybridSmartVerticesNonSatSingleServer";
        const verticesSat = "UnitTestDumpHybridSmartVerticesSatSingleServer";
        const edges = "UnitTestDumpHybridSmartEdgesSingleServer";
        const orphans = "UnitTestDumpHybridSmartOrphansSingleServer";

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(hybridGraphName));

        const graph = g.document(hybridGraphName);
        assertTrue(graph.isSmart);
        assertFalse(graph.isSatellite);
        assertEqual(graph.replicationFactor, 2);
        assertEqual(graph.numberOfShards, 1);
        assertEqual(graph.smartGraphAttribute, 'value');

        assertEqual(graph.edgeDefinitions, [
          {
            "collection": edges,
            "from": [verticesSat],
            "to": [verticesNonSat]
          }
        ]);
        assertEqual(graph.orphanCollections, [orphans]);

        // Test smart collections
        for (const collName of [verticesNonSat, orphans]) {
          const vColl = getCollection(collName);
          const properties = vColl.properties();
          assertTrue(properties.isSmart);
          assertEqual(properties.isDisjoint, false);
          assertEqual(properties.smartGraphAttribute, 'value');
          assertEqual(properties.numberOfShards, 1);
          assertEqual(properties.replicationFactor, 2);
          if (isCluster) {
            assertEqual(properties.shardingStrategy, 'hash');
          }
        }

        // Test satellite collections
        for (const collName of [verticesSat]) {
          const vColl = getCollection(collName);
          const properties = vColl.properties();
          assertFalse(properties.isSmart);
          assertEqual(properties.isDisjoint, false);
          assertEqual(properties.smartGraphAttribute, undefined);
          assertEqual(properties.numberOfShards, 1);
          assertEqual(properties.replicationFactor, 'satellite');
          if (isCluster) {
            assertEqual(properties.shardingStrategy, 'hash');
          }
        }

        // Test edge collection
        const eColl = getCollection(edges);
        const properties = eColl.properties();
        assertFalse(properties.isSmart);
        assertFalse(properties.isDisjoint);
        assertEqual(properties.numberOfShards, 1);
        assertEqual(properties.replicationFactor, 'satellite');
        if (isCluster) {
          assertEqual(properties.shardingStrategy, 'hash');
        }

        // test data
        const collVerticesNonSat = getCollection(verticesNonSat);
        const collVerticesSat = getCollection(verticesSat);
        const collOrphans = getCollection(orphans);

        // only self-loops are saved as the others have different values of the 'value' key, which is the smartGraphAttribute field
        assertEqual(eColl.count(), 100);

        assertEqual(collVerticesNonSat.count(), 50);
        assertEqual(collVerticesSat.count(), 50);
        assertEqual(collOrphans.count(), 100);

        for (const edge of eColl.toArray()) {
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);
        }
      },

      testHybridDisjointSmartGraphSingleServer: function () {
        if (!isEnterprise) {
          return;
        }
        const hybridGraphName = "UnitTestDumpHybridDisjointSmartGraphSingleServer";
        const verticesANonSat = "UnitTestDumpHybridDisjointSmartVerticesANonSatSingleServer";
        const verticesBNonSat = "UnitTestDumpHybridDisjointSmartVerticesBNonSatSingleServer";
        const verticesSat = "UnitTestDumpHybridDisjointSmartVerticesSatSingleServer";
        const edgesSatA = "UnitTestDumpHybridSDisjointmartEdgesSatASingleServer";
        const edgesAB = "UnitTestDumpHybridSDisjointmartEdgesABSingleServer";
        const orphans = "UnitTestDumpHybridSDisjointmartOrphansSingleServer";

        const g = getCollection('_graphs');
        assertTrue(!!g.exists(hybridGraphName));

        // check graph properties
        const graph = g.document(hybridGraphName);
        assertEqual(graph.isSmart, true);
        assertFalse(graph.isSatellite);
        assertEqual(graph.replicationFactor, 2);
        assertEqual(graph.numberOfShards, 1);
        assertEqual(graph.smartGraphAttribute, 'value');

        // check edge definitions
        const cmp = function (a, b) {
          return a.collection < b.collection ? 1 : a.collection === b.collection ? 0 : -1;
        };
        assertEqual(graph.edgeDefinitions.sort(cmp), [
          {
            "collection": edgesSatA,
            "from": [verticesSat],
            "to": [verticesANonSat]
          },
          {
            "collection": edgesAB,
            "from": [verticesANonSat],
            "to": [verticesBNonSat]
          }
        ].sort(cmp));

        // check orphan collections
        assertEqual(graph.orphanCollections, [orphans]);

        // check properties of vertex collections
        const verticesANonSatCol = getCollection(verticesANonSat);
        const verticesBNonSatCol = getCollection(verticesBNonSat);
        const verticesSatCol = getCollection(verticesSat);
        const orphansCol = getCollection(orphans);

        // properties common for all vertex collections (smart & satellite)
        for (const collection of [verticesANonSatCol, verticesBNonSatCol, verticesSatCol, orphansCol]) {
          assertEqual(collection.properties().numberOfShards, 1);
        }

        // properties common for all vertex non-satellite collection
        for (const collection of [verticesANonSatCol, verticesBNonSatCol, orphansCol]) {
          assertTrue(collection.properties().isSmart);
          assertTrue(collection.properties().isDisjoint);
          assertEqual(collection.properties().smartGraphAttribute, 'value');
          assertEqual(collection.properties().replicationFactor, 2);
          if (isCluster) {
            assertEqual(collection.properties().shardingStrategy, 'hash', `Failed for collection: "${collection.name()}".`);
          }
        }

        // properties only for verticesSatProps (satellite collection)
        const verticesSatProps = verticesSatCol.properties();
        assertFalse(verticesSatProps.isSmart);
        assertFalse(verticesSatProps.isDisjoint);
        assertEqual(verticesSatProps.smartGraphAttribute, undefined);
        assertEqual(verticesSatProps.replicationFactor, 'satellite');
        if (isCluster) {
          assertEqual(verticesSatCol.properties().shardingStrategy, 'hash', `Failed for collection: "${verticesSatCol.name()}".`);
        }

        // check properties of edge collections
        const eCollSatA = getCollection(edgesSatA);
        const eCollAB = getCollection(edgesAB);
        for (const properties of [eCollSatA.properties(), eCollAB.properties()]) {
          assertTrue(properties.isSmart);
          assertTrue(properties.isDisjoint);

          if (isCluster) {
            assertEqual(properties.numberOfShards, 0);
            assertEqual(properties.shardingStrategy, 'enterprise-hash-smart-edge');
          } else {
            assertEqual(properties.numberOfShards, 1);
          }
          assertEqual(properties.replicationFactor, 2);
        }

        // test data
        const collVerticesANonSat = getCollection(verticesANonSat);
        const collVerticesBNonSat = getCollection(verticesBNonSat);
        const collVerticesSat = getCollection(verticesSat);
        const collOrphans = getCollection(orphans);

        // see explanation in the documentation to createHybridDisjointSmartGraphSingleServer() in file dump-set-common.inc
        assertEqual(eCollSatA.count(), 100);
        assertEqual(eCollAB.count(), 100);

        assertEqual(collVerticesANonSat.count(), 50);
        assertEqual(collVerticesBNonSat.count(), 50);
        assertEqual(collVerticesSat.count(), 50);
        // |verticesSat| + |verticesANonSat|. Faked: not really orphans but this is not tested
        assertEqual(collOrphans.count(), 100);

        for (const edge of eCollSatA.toArray().concat(eCollAB.toArray())) {
          assertTrue(!!edge._from);
          assertTrue(!!edge._to);
        }
      }
    };
  };
}());
