/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global getOptions, assertEqual, assertTrue, assertNotNull, assertNotUndefined, arango */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2023 ArangoDB GmbH, Cologne, Germany
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
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Julia Puget
// //////////////////////////////////////////////////////////////////////////////

const jwtSecret = 'abc';

if (getOptions === true) {
  return {
    'server.enable-telemetrics-api': 'true',
    'server.authentication': 'true',
    'server.jwt-secret': jwtSecret,
  };
}

let jsunity = require('jsunity');
let internal = require('internal');
const FoxxManager = require('@arangodb/foxx/manager');
const path = require('path');
const basePath = path.resolve(internal.pathForTesting('common'), 'test-data', 'apps', 'redirect');
let arangodb = require('@arangodb');
const users = require('@arangodb/users');
const isCluster = require("internal").isCluster();
const isEnterprise = require("internal").isEnterprise();
let smartGraph = null;
if (isEnterprise) {
  smartGraph = require("@arangodb/smart-graph");
}
const userName = "abc";
const databaseName = "databaseTest";
const cn = "testCollection";
const cn2 = "smartGraphTestCollection";
const cn3 = "edgeTestCollection";
const vn1 = "testVertex1";
const vn2 = "testVertex2";

let db = arangodb.db;

function createUser() {
  users.save(userName, '123', true);
  users.grantDatabase(userName, "_system", 'ro');
  users.reload();
}

function removeUser() {
  users.remove(userName);
}

function getTelemetricsResult() {
  let res;
  let numSecs = 0.5;
  while (true) {
    res = arango.getTelemetricsInfo();
    if (res !== undefined || numSecs >= 16) {
      break;
    }
    internal.sleep(numSecs);
    numSecs *= 2;
  }
  assertNotUndefined(res);
  return res;
}

function getTelemetricsSentToEndpoint() {
  let res;
  let numSecs = 0.5;
  while (true) {
    res = arango.sendTelemetricsToEndpoint("/test-redirect/redirect");
    if (res !== undefined || numSecs >= 16) {
      break;
    }
    internal.sleep(numSecs);
    numSecs *= 2;
  }
  assertNotUndefined(res);
  return res;
}

function parseIndexes(idxs) {
  idxs.forEach(idx => {
    assertTrue(idx.hasOwnProperty("mem"));
    assertTrue(idx.hasOwnProperty("type"));
    assertTrue(idx.hasOwnProperty("sparse"));
    assertTrue(idx.hasOwnProperty("unique"));
  });
}

//n_smart_colls, for now, doesn't give precise smart graph values, as it relies on the collection's
// isSmart() function and collections with no shards, such as the edge collection from the smart graph
// are not seen from the db server's methods, so should be in the coordinator and run a query on the database
function parseCollections(colls) {
  colls.forEach(coll => {
    assertTrue(coll.hasOwnProperty("n_shards"));
    assertTrue(coll.hasOwnProperty("rep_factor"));
    assertTrue(coll.hasOwnProperty("plan_id"));
    assertTrue(coll.hasOwnProperty("n_docs"));
    assertTrue(coll.hasOwnProperty("type"));
    assertTrue(coll.hasOwnProperty("n_edge"));
    assertTrue(coll.hasOwnProperty("n_fulltext"));
    assertTrue(coll.hasOwnProperty("n_geo"));
    assertTrue(coll.hasOwnProperty("n_hash"));
    assertTrue(coll.hasOwnProperty("n_inverted"));
    assertTrue(coll.hasOwnProperty("n_iresearch"));
    assertTrue(coll.hasOwnProperty("n_persistent"));
    assertTrue(coll.hasOwnProperty("n_primary"));
    assertTrue(coll.hasOwnProperty("n_skiplist"));
    assertTrue(coll.hasOwnProperty("n_ttl"));
    assertTrue(coll.hasOwnProperty("n_unknown"));
    assertTrue(coll.hasOwnProperty("n_zkd"));
    const idxs = coll["idxs"];
    parseIndexes(idxs);
  });
}

function parseDatabases(databases) {
  databases.forEach(database => {
    assertTrue(database.hasOwnProperty("colls"));
    const colls = database["colls"];
    parseCollections(colls);
    assertTrue(database.hasOwnProperty("single_shard"));
    assertTrue(database.hasOwnProperty("n_views"));
    assertTrue(database.hasOwnProperty("n_disjoint_smart_colls"));
    assertTrue(database.hasOwnProperty("n_doc_colls"));
    assertTrue(database.hasOwnProperty("n_edge_colls"));
    assertTrue(database.hasOwnProperty("n_smart_colls"));
  });

}

function parseServers(servers) {
  servers.forEach(host => {
    assertTrue(host.hasOwnProperty("role"));
    assertTrue(host.hasOwnProperty("maintenance"));
    assertTrue(host.hasOwnProperty("read_only"));
    assertTrue(host.hasOwnProperty("version"));
    assertTrue(host.hasOwnProperty("build"));
    assertTrue(host.hasOwnProperty("os"));
    assertTrue(host.hasOwnProperty("platform"));
    assertTrue(host.hasOwnProperty("phys_mem"));
    assertTrue(host["phys_mem"].hasOwnProperty("value"));
    assertTrue(host["phys_mem"].hasOwnProperty("overridden"));
    assertTrue(host.hasOwnProperty("n_cores"));
    assertTrue(host["n_cores"].hasOwnProperty("value"));
    assertTrue(host["n_cores"].hasOwnProperty("overridden"));
    assertTrue(host.hasOwnProperty("process_stats"));
    let stats = host["process_stats"];
    assertTrue(stats.hasOwnProperty("process_uptime"));
    assertTrue(stats.hasOwnProperty("n_threads"));
    assertTrue(stats.hasOwnProperty("virtual_size"));
    assertTrue(stats.hasOwnProperty("resident_set_size"));
    if (host.role !== "COORDINATOR") {
      assertTrue(host.hasOwnProperty("engine_stats"));
      stats = host["engine_stats"];
      assertTrue(stats.hasOwnProperty("cache_limit"));
      assertTrue(stats.hasOwnProperty("cache_allocated"));
      assertTrue(stats.hasOwnProperty("rocksdb_estimate_num_keys"));
      assertTrue(stats.hasOwnProperty("rocksdb_estimate_live_data_size"));
      assertTrue(stats.hasOwnProperty("rocksdb_live_sst_files_size"));
      assertTrue(stats.hasOwnProperty("rocksdb_block_cache_capacity"));
      assertTrue(stats.hasOwnProperty("rocksdb_block_cache_usage"));
      assertTrue(stats.hasOwnProperty("rocksdb_free_disk_space"));
      assertTrue(stats.hasOwnProperty("rocksdb_total_disk_space"));
    }
  });

}

function assertForTelemetricsResponse(telemetrics) {
  assertTrue(telemetrics.hasOwnProperty("deployment"));
  const deployment = telemetrics["deployment"];
  assertTrue(deployment.hasOwnProperty("type"));
  assertTrue(deployment.hasOwnProperty("license"));
  assertTrue(deployment.hasOwnProperty("servers"));
  const servers = deployment["servers"];
  parseServers(servers);
  if (isCluster) {
    assertEqual(deployment["type"], "cluster");
  } else {
    assertEqual(deployment["type"], "single");
  }
  if (deployment.hasOwnProperty("shards_statistics")) {
    const statistics = deployment["shards_statistics"];
    assertTrue(statistics.hasOwnProperty("databases"));
    assertTrue(statistics.hasOwnProperty("collections"));
    assertTrue(statistics.hasOwnProperty("shards"));
    assertTrue(statistics.hasOwnProperty("leaders"));
    assertTrue(statistics.hasOwnProperty("realLeaders"));
    assertTrue(statistics.hasOwnProperty("followers"));
    assertTrue(statistics.hasOwnProperty("servers"));
  }
  assertTrue(deployment.hasOwnProperty("date"));
  assertTrue(deployment.hasOwnProperty("databases"));
  const databases = deployment["databases"];
  parseDatabases(databases);
}


function telemetricsShellReconnectSmartGraphTestsuite() {

  return {

    setUpAll: function () {
      assertNotNull(smartGraph);
      try {
        smartGraph._drop(cn2, true);
      } catch (err) {
      }
      db._create(cn);
      const vn = "verticesCollection";
      const en = "edgesCollection";
      const graphRelation = [smartGraph._relation(en, vn, vn)];
      smartGraph._create(cn2, graphRelation, null, {smartGraphAttribute: "value1"});
      smartGraph._graph(cn2);
    },

    tearDownAll: function () {
      db._drop(cn);
      try {
        smartGraph._drop(cn2, true);
      } catch (err) {
      }
    },

    testTelemetricsShellRequestByUserAuthorized: function () {
      try {
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        let telemetrics = getTelemetricsResult();
        assertForTelemetricsResponse(telemetrics);
        let deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 1);


        db._createDatabase(databaseName);
        db._useDatabase(databaseName);

        let numShards = 1;
        if (isCluster) {
          numShards = 2;
        }
        db._create(cn, {numberOfShards: numShards, replicationFactor: 1});
        let docs = [];
        for (let i = 0; i < 2000; ++i) {
          docs.push({value: i + 1, name: "abc"});
        }
        db[cn].insert(docs);
        db[cn].ensureIndex({type: "persistent", fields: ["value"], name: "persistentIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["geo"], name: "geoIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["otherGeo"], name: "geoIdx2"});

        db._createView('testView1', 'arangosearch', {});
        db._useDatabase("_system");

        arango.restartTelemetrics();

        telemetrics = getTelemetricsResult();
        assertForTelemetricsResponse(telemetrics);
        deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 2);
        const databases = deployment["databases"];
        const db1Idx = databases[0]["n_views"] === 1 ? 0 : 1;
        assertEqual(databases[db1Idx]["n_views"], 1);
        let numColls = 0;
        databases.forEach((database, idx) => {
          let totalNumDocs = 0;
          if (idx === db1Idx) {
            // there are already the 8 system collections in each database + 1 created here
            assertEqual(database["n_doc_colls"], 10);
            database["colls"].forEach(coll => {
              const nDocs = coll["n_docs"];
              totalNumDocs += nDocs;
              if (nDocs === 2000) {
                assertEqual(coll["n_shards"], numShards);
                assertEqual(coll["rep_factor"], 1);
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 1);
                assertEqual(coll["n_geo"], 2);
                numColls++;
                assertEqual(coll.idxs.length, 4);
              } else {
                assertEqual(nDocs, 0);
                //system collections have replication factor 2
                if (!isCluster) {
                  assertEqual(coll["rep_factor"], 1);
                }
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 0);
                assertEqual(coll["n_geo"], 0);
              }
            });
            assertEqual(totalNumDocs, 2000);
          } else {
            //includes the collections created for the smart graph
            assertEqual(database["n_doc_colls"], 15);
            database["colls"].forEach(coll => {
              assertEqual(coll["n_primary"], 1);
              assertEqual(coll["n_persistent"], 0);
              if (!isCluster) {
                assertEqual(coll["rep_factor"], 1);
              }
              assertEqual(coll["n_geo"], 0);
            });
          }
        });
      } finally {
        db._useDatabase("_system");
        try {
          db._dropDatabase(databaseName);
        } catch (err) {

        }
      }
    },

    testTelemetricsShellRequestByUserNotAuthorized: function () {
      try {
        createUser();
        arango.reconnect(arango.getEndpoint(), '_system', userName, "123");
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        const res = getTelemetricsResult();
        assertTrue(res.hasOwnProperty("errorNum"));
        assertTrue(res.hasOwnProperty("errorMessage"));
        assertEqual(res.errorNum, internal.errors.ERROR_HTTP_FORBIDDEN.code);
        assertTrue(res.errorMessage.includes("insufficient permissions"));
      } finally {
        arango.reconnect(arango.getEndpoint(), '_system', 'root', '');
        if (users.exists(userName)) {
          removeUser();
        }
      }
    },
  };
}

function telemetricsShellReconnectGraphTestsuite() {

  return {

    setUpAll: function () {
      db._create(cn);
      let coll = db._createEdgeCollection(cn3, {numberOfShards: 2});
      coll.insert({_from: vn1 + "/test1", _to: vn2 + "/test2"});
    },

    tearDownAll: function () {
      db._drop(cn);
      db._drop(cn3);
    },

    testTelemetricsShellRequestByUserAuthorized2: function () {
      try {
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        let telemetrics = getTelemetricsResult();
        assertForTelemetricsResponse(telemetrics);
        let deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 1);


        db._createDatabase(databaseName);
        db._useDatabase(databaseName);

        let numShards = 1;
        if (isCluster) {
          numShards = 2;
        }
        db._create(cn, {numberOfShards: numShards, replicationFactor: 1});
        let docs = [];
        for (let i = 0; i < 2000; ++i) {
          docs.push({value: i + 1, name: "abc"});
        }
        db[cn].insert(docs);
        db[cn].ensureIndex({type: "persistent", fields: ["value"], name: "persistentIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["geo"], name: "geoIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["otherGeo"], name: "geoIdx2"});

        db._createView('testView1', 'arangosearch', {});

        db._useDatabase("_system");

        arango.restartTelemetrics();

        telemetrics = getTelemetricsResult();
        assertForTelemetricsResponse(telemetrics);
        deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 2);
        const databases = deployment["databases"];
        const db1Idx = databases[0]["n_views"] === 1 ? 0 : 1;
        assertEqual(databases[db1Idx]["n_views"], 1);
        let numColls = 0;
        databases.forEach((database, idx) => {
          let totalNumDocs = 0;
          if (idx === db1Idx) {
            // there are already the 9 system collections in the database + 1 created here
            assertEqual(database["n_doc_colls"], 10);
            database["colls"].forEach(coll => {
              const nDocs = coll["n_docs"];
              totalNumDocs += nDocs;
              if (nDocs === 2000) {
                assertEqual(coll["n_shards"], numShards);
                assertEqual(coll["rep_factor"], 1);
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 1);
                assertEqual(coll["n_geo"], 2);
                numColls++;
                assertEqual(coll.idxs.length, 4);
              } else {
                assertEqual(nDocs, 0);
                //system collections have replication factor 2
                if (!isCluster) {
                  assertEqual(coll["rep_factor"], 1);
                }
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 0);
                assertEqual(coll["n_geo"], 0);
              }
            });
            assertEqual(totalNumDocs, 2000);
          } else {
            // there are already 12 collections in the _system database + 2 created here
            assertEqual(database["n_doc_colls"], 14);
            assertEqual(database["n_edge_colls"], 1);
            database["colls"].forEach(coll => {
              assertEqual(coll["n_primary"], 1);
              assertEqual(coll["n_persistent"], 0);
              if (!isCluster) {
                assertEqual(coll["rep_factor"], 1);
              }
              assertEqual(coll["n_geo"], 0);
            });
          }
        });
      } finally {
        db._useDatabase("_system");
        try {
          db._dropDatabase(databaseName);
        } catch (err) {

        }
      }
    },
  };
}


function telemetricsApiReconnectSmartGraphTestsuite() {

  return {

    setUpAll: function () {
      assertNotNull(smartGraph);
      try {
        smartGraph._drop(cn2, true);
      } catch (err) {
      }
      db._create(cn);
      const vn = "verticesCollection";
      const en = "edgesCollection";
      const graphRelation = [smartGraph._relation(en, vn, vn)];
      smartGraph._create(cn2, graphRelation, null, {smartGraphAttribute: "value1", replicationFactor: 1});
      smartGraph._graph(cn2);
    },

    tearDownAll: function () {
      db._drop(cn);
      try {
        smartGraph._drop(cn2, true);
      } catch (err) {
      }
    },

    testTelemetricsApiRequestByUserAuthorized: function () {
      try {
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        let telemetrics = arango.GET("/_admin/telemetrics");
        assertForTelemetricsResponse(telemetrics);
        let deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 1);


        db._createDatabase(databaseName);
        db._useDatabase(databaseName);

        let numShards = 1;
        if (isCluster) {
          numShards = 2;
        }
        db._create(cn, {numberOfShards: numShards, replicationFactor: 1});
        let docs = [];
        for (let i = 0; i < 2000; ++i) {
          docs.push({value: i + 1, name: "abc"});
        }
        db[cn].insert(docs);
        db[cn].ensureIndex({type: "persistent", fields: ["value"], name: "persistentIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["geo"], name: "geoIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["otherGeo"], name: "geoIdx2"});

        db._createView('testView1', 'arangosearch', {});
        db._useDatabase("_system");
        telemetrics = arango.GET("/_admin/telemetrics");

        assertForTelemetricsResponse(telemetrics);
        deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 2);
        const databases = deployment["databases"];
        const db1Idx = databases[0]["n_views"] === 1 ? 0 : 1;
        assertEqual(databases[db1Idx]["n_views"], 1);
        let numColls = 0;
        databases.forEach((database, idx) => {
          let totalNumDocs = 0;
          if (idx === db1Idx) {
            // there are already the 8 system collections in each database + 1 created here
            assertEqual(database["n_doc_colls"], 10);
            database["colls"].forEach(coll => {
              const nDocs = coll["n_docs"];
              totalNumDocs += nDocs;
              if (nDocs === 2000) {
                assertEqual(coll["n_shards"], numShards);
                assertEqual(coll["rep_factor"], 1);
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 1);
                assertEqual(coll["n_geo"], 2);
                numColls++;
                assertEqual(coll.idxs.length, 4);
              } else {
                assertEqual(nDocs, 0);
                //system collections would have replication factor 2
                if (!isCluster) {
                  assertEqual(coll["rep_factor"], 1);
                }
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 0);
                assertEqual(coll["n_geo"], 0);
              }
            });
            assertEqual(totalNumDocs, 2000);
          } else {
            //includes the collections created for the smart graph
            assertEqual(database["n_doc_colls"], 15);
            database["colls"].forEach(coll => {
              assertEqual(coll["n_primary"], 1);
              assertEqual(coll["n_persistent"], 0);
              if (!isCluster) {
                assertEqual(coll["rep_factor"], 1);
              }
              assertEqual(coll["n_geo"], 0);
            });
          }
        });
      } finally {
        db._useDatabase("_system");
        try {
          db._dropDatabase(databaseName);
        } catch (err) {

        }
      }
    },

    testTelemetricsApiRequestByUserNotAuthorized: function () {
      try {
        createUser();
        arango.reconnect(arango.getEndpoint(), '_system', userName, "123");
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        const res = arango.GET("/_admin/telemetrics");
        assertTrue(res.hasOwnProperty("errorNum"));
        assertTrue(res.hasOwnProperty("errorMessage"));
        assertEqual(res.errorNum, internal.errors.ERROR_HTTP_FORBIDDEN.code);
        assertTrue(res.errorMessage.includes("insufficient permissions"));
      } finally {
        arango.reconnect(arango.getEndpoint(), '_system', 'root', '');
        if (users.exists(userName)) {
          removeUser();
        }
      }
    },


    testTelemetricsInsertingEndpointReqBodyAsDocument: function () {
      arango.disableAutomaticallySendTelemetricsToEndpoint();
      arango.startTelemetrics();
      const telemetrics = arango.GET("/_admin/telemetrics");
      assertForTelemetricsResponse(telemetrics);
      const doc = arango.POST("/_api/document/" + cn, telemetrics);
      assertTrue(doc.hasOwnProperty("_id"));
      assertTrue(doc.hasOwnProperty("_key"));
      assertTrue(doc.hasOwnProperty("_rev"));
      assertForTelemetricsResponse(db[cn].document(doc["_id"]));

    },

  };
}

function telemetricsApiReconnectGraphTestsuite() {

  return {

    setUpAll: function () {
      db._create(cn);
      let coll = db._createEdgeCollection(cn3, {numberOfShards: 2});
      coll.insert({_from: vn1 + "/test1", _to: vn2 + "/test2"});
    },

    tearDownAll: function () {
      db._drop(cn);
      db._drop(cn3);
    },

    testTelemetricsApiRequestByUserAuthorized2: function () {
      try {
        arango.disableAutomaticallySendTelemetricsToEndpoint();
        arango.startTelemetrics();
        let telemetrics = arango.GET("/_admin/telemetrics");
        assertForTelemetricsResponse(telemetrics);
        let deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 1);


        db._createDatabase(databaseName);
        db._useDatabase(databaseName);

        let numShards = 1;
        if (isCluster) {
          numShards = 2;
        }
        db._create(cn, {numberOfShards: numShards, replicationFactor: 1});
        let docs = [];
        for (let i = 0; i < 2000; ++i) {
          docs.push({value: i + 1, name: "abc"});
        }
        db[cn].insert(docs);
        db[cn].ensureIndex({type: "persistent", fields: ["value"], name: "persistentIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["geo"], name: "geoIdx1"});
        db[cn].ensureIndex({type: "geo", geoJson: true, fields: ["otherGeo"], name: "geoIdx2"});

        db._createView('testView1', 'arangosearch', {});
        db._useDatabase("_system");
        telemetrics = arango.GET("/_admin/telemetrics");

        assertForTelemetricsResponse(telemetrics);
        deployment = telemetrics["deployment"];
        assertEqual(deployment["databases"].length, 2);
        const databases = deployment["databases"];
        const db1Idx = databases[0]["n_views"] === 1 ? 0 : 1;
        assertEqual(databases[db1Idx]["n_views"], 1);
        let numColls = 0;
        databases.forEach((database, idx) => {
          let totalNumDocs = 0;
          if (idx === db1Idx) {
            // there are already the 8 system collections in each database + 1 created here
            assertEqual(database["n_doc_colls"], 10);
            database["colls"].forEach(coll => {
              const nDocs = coll["n_docs"];
              totalNumDocs += nDocs;
              if (nDocs === 2000) {
                assertEqual(coll["n_shards"], numShards);
                assertEqual(coll["rep_factor"], 1);
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 1);
                assertEqual(coll["n_geo"], 2);
                numColls++;
                assertEqual(coll.idxs.length, 4);
              } else {
                assertEqual(nDocs, 0);
                //system collections would have replication factor 2
                if (!isCluster) {
                  assertEqual(coll["rep_factor"], 1);
                }
                assertEqual(coll["n_primary"], 1);
                assertEqual(coll["n_persistent"], 0);
                assertEqual(coll["n_geo"], 0);
              }
            });
            assertEqual(totalNumDocs, 2000);
          } else {
            assertEqual(database["n_doc_colls"], 14);
            assertEqual(database["n_edge_colls"], 1);
            database["colls"].forEach(coll => {
              assertEqual(coll["n_primary"], 1);
              assertEqual(coll["n_persistent"], 0);
              if (!isCluster) {
                assertEqual(coll["rep_factor"], 1);
              }
              assertEqual(coll["n_geo"], 0);
            });
          }
        });
      } finally {
        db._useDatabase("_system");
        try {
          db._dropDatabase(databaseName);
        } catch (err) {

        }
      }
    },
  };
}


function telemetricsSendToEndpointRedirectTestsuite() {

  const mount = '/test-redirect';

  return {

    setUpAll: function () {
      try {
        FoxxManager.uninstall(mount, {force: true});
      } catch (err) {
      }
      try {
        FoxxManager.install(basePath, mount);
      } catch (err) {
      }
      db._create(cn);
      let coll = db._createEdgeCollection(cn3, {numberOfShards: 2});
      coll.insert({_from: vn1 + "/test1", _to: vn2 + "/test2"});
    },

    tearDownAll: function () {
      db._drop(cn);
      db._drop(cn3);
      try {
        FoxxManager.uninstall(mount, {force: true});
      } catch (err) {
      }
    },

    testTelemetricsSendToEndpointWithRedirection: function () {
      arango.disableAutomaticallySendTelemetricsToEndpoint();
      arango.startTelemetrics();
      getTelemetricsResult();
      const telemetrics = getTelemetricsSentToEndpoint();
      assertForTelemetricsResponse(telemetrics);
    },

  };
}

if (isEnterprise) {
  jsunity.run(telemetricsShellReconnectSmartGraphTestsuite);
  jsunity.run(telemetricsApiReconnectSmartGraphTestsuite);
}
jsunity.run(telemetricsShellReconnectGraphTestsuite);
jsunity.run(telemetricsApiReconnectGraphTestsuite);
jsunity.run(telemetricsSendToEndpointRedirectTestsuite);


return jsunity.done();
