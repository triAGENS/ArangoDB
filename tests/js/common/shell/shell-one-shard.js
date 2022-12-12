/*jshint globalstrict:false, strict:false */
/*global assertTrue, assertEqual, assertNotEqual, assertMatch, assertNull, fail */

////////////////////////////////////////////////////////////////////////////////
/// @brief test the collection interface
///
/// @file
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
/// @author Copyright 2019, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const internal = require('internal');
const ERRORS = arangodb.errors;
const isEnterprise = internal.isEnterprise();
const isCluster = internal.isCluster();
const request = require('@arangodb/request');

const defaultReplicationFactor = db._properties().replicationFactor;

function getEndpointsByType(type) {
  const isType = (d) => (d.instanceRole === type);
  const toEndpoint = (d) => (d.endpoint);
  const endpointToURL = (endpoint) => {
    if (endpoint.substr(0, 6) === 'ssl://') {
      return 'https://' + endpoint.substr(6);
    }
    let pos = endpoint.indexOf('://');
    if (pos === -1) {
      return 'http://' + endpoint;
    }
    return 'http' + endpoint.substr(pos);
  };

  const instanceManager = JSON.parse(internal.env.INSTANCEINFO);
  return instanceManager.arangods.filter(isType)
                              .map(toEndpoint)
                              .map(endpointToURL);
}

function checkDBServerSharding(db, expected) {
  // connect to all db servers and check if they picked up the
  // "sharding" attribute correctly
  if (!require('@arangodb').isServer) {
    // request module can only be used inside arangosh tests
    let endpoints = getEndpointsByType("dbserver");
    assertTrue(endpoints.length > 0);
    endpoints.forEach((ep) => {
      let res = request.get({ url: ep + "/_db/" + encodeURIComponent(db) + "/_api/database/current" });
      assertEqual(200, res.status);
      assertEqual(expected, res.json.result.sharding);
    });
  }
}

function OneShardPropertiesSuite () {
  var dn = "UnitTestsDB";

  return {
    setUp: function () {
      try {
        db._useDatabase("_system");
        db._dropDatabase(dn);
      } catch (ex) {
      }
    },

    tearDown: function () {
      try {
        db._useDatabase("_system");
        db._dropDatabase(dn);
      } catch (ex) {
      }
    },

    testDefaultValues : function () {
      assertTrue(db._createDatabase(dn));
      db._useDatabase(dn);
      let props = db._properties();
      if (isCluster) {
        assertEqual(props.sharding, "");
        assertEqual(props.replicationFactor, defaultReplicationFactor);
      } else {
        assertEqual(props.sharding, undefined);
        assertEqual(props.replicationFactor, undefined);
      }
    },
    
    testDefaultValuesOverridden : function () {
      assertTrue(db._createDatabase(dn, { replicationFactor: 2, writeConcern: 2, sharding: "single" }));
      db._useDatabase(dn);
      let props = db._properties();
      if (isCluster) {
        assertEqual(props.sharding, "single");
        assertEqual(props.replicationFactor, 2);
        assertEqual(props.writeConcern, 2);

        try {
	  // Disallow using a different distributeShardsLike
          let c = db._create("test", { distributeShardsLike: "" });
	  fail();
	} catch (err) {
          assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
	}
        let c = db._create("test", { writeConcern: 1, replicationFactor: 1, numberOfShards: 2 });
        props = c.properties();
        assertEqual(2, props.writeConcern);
        assertEqual(2, props.replicationFactor);
        assertEqual(1, props.numberOfShards);

        checkDBServerSharding(dn, "single");
      } else {
        assertEqual(props.sharding, undefined);
        assertEqual(props.replicationFactor, undefined);
        assertEqual(props.writeConcern, undefined);
      }
    },
    
    testShardingFlexible : function () {
      assertTrue(db._createDatabase(dn, { sharding: "flexible" }));
      db._useDatabase(dn);
      let props = db._properties();
      if (isCluster) {
        assertEqual(props.sharding, "");
        assertEqual(props.replicationFactor, defaultReplicationFactor);
        
        checkDBServerSharding(dn, "");
      } else {
        assertEqual(props.sharding, undefined);
        assertEqual(props.replicationFactor, undefined);
      }
    },

    testDeviatingWriteConcernAndMinReplicationFactorForDatabase : function () {
      if (!isCluster) {
        return;
      }
      try {
        db._createDatabase(dn, { replicationFactor: 2, minReplicationFactor: 1, writeConcern: 2, sharding: "flexible" });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
        
      assertTrue(db._createDatabase(dn, { replicationFactor: 2, minReplicationFactor: 2, writeConcern: 2, sharding: "flexible" }));
    },
    
    testNormalDBAndTooManyServers : function () {
      if (!isCluster) {
        return;
      }
      try {
        db._createDatabase(dn, { replicationFactor : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
      try {
        db._createDatabase(dn, { writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
    },
    
    testNormalDBAndTooManyServers2 : function () {
      if (!isCluster) {
        return;
      }
      db._createDatabase(dn, { replicationFactor : 2 });
      db._useDatabase(dn);
      try {
        db._create("oneshardcol", { replicationFactor : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
      try {
        db._create("oneshardcol", { writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },
    
    testNormalDBAndTooManyServers3 : function () {
      if (!isCluster) {
        return;
      }
      db._createDatabase(dn, { writeConcern: 2, replicationFactor : 2 });
      db._useDatabase(dn);
      try {
        db._create("oneshardcol", { replicationFactor : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
      try {
        db._create("oneshardcol", { writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },
    
    testOneShardDBAndTooManyServers : function () {
      if (!isCluster) {
        return;
      }
      try {
        db._createDatabase(dn, { sharding : "single", replicationFactor : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
      try {
        db._createDatabase(dn, { sharding : "single", writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_CLUSTER_INSUFFICIENT_DBSERVERS.code, err.errorNum);
      }
    },
    
    testOneShardDBAndTooManyServers2 : function () {
      if (!isCluster) {
        return;
      }
      db._createDatabase(dn, { sharding : "single", replicationFactor : 2 });
      db._useDatabase(dn);
      // ReplicationFactor is overwritten by distributeShardsLike
      let c = db._create("oneshardcol", { replicationFactor : 5 });
      assertEqual(c.properties().replicationFactor, 2);
      db._drop("oneshardcol");
      try {
        // WriteConcern is specific to collection and is now greater than replicationFactor, which is disallowed
        db._create("oneshardcol", { writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },
    
    testOneShardDBAndTooManyServers3 : function () {
      if (!isCluster) {
        return;
      }
      db._createDatabase(dn, { sharding : "single", writeConcern: 2, replicationFactor : 2 });
      db._useDatabase(dn);
      // ReplicationFactor is overwritten by distributeShardsLike
      let c = db._create("oneshardcol", { replicationFactor : 5 });
      assertEqual(c.properties().replicationFactor, 2);
      assertEqual(c.properties().writeConcern, 2);
      try {
        // WriteConcern is specific to collection and is now greater than replicationFactor, which is disallowed
        db._create("oneshardcol", { writeConcern : 5 });
        fail();
      } catch (err) {
        assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
      }
    },
    
    testOneShardDBAndOverrides : function () {
      assertTrue(db._createDatabase(dn, { sharding : "single", replicationFactor : 2 }));

      db._useDatabase(dn);
      let props = db._properties();
      if (!isCluster) {
        assertEqual(props.sharding, undefined);
        assertEqual(props.replicationFactor, undefined);
      } else {
        assertEqual(props.sharding, "single", props);
        assertEqual(props.replicationFactor, 2, props);

        {
          let col = db._create("oneshardcol");
          let colProperties = col.properties();
          let graphsProperties = db._collection("_graphs").properties();

          if (isCluster && isEnterprise) {
            assertEqual(colProperties.distributeShardsLike, "_graphs");
            assertEqual(colProperties.replicationFactor, graphsProperties.replicationFactor);
          }
        }

        if (isCluster) {
          try {
            // We set distributeShardsLike to an illegal value
            db._create("overrideOneShardCollection", {distributeShardsLike: ""});
            fail();
          } catch (err) {
            assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
          }
        } else {
          // Should be allowed on single server
          db._create("overrideOneShardCollection", {distributeShardsLike: ""});
        }

        {
          // we want to create a normal collection and have a different replication factor
          let nonDefaultReplicationFactor = 1;
          assertNotEqual(db._properties().replicationFactor, nonDefaultReplicationFactor);
          let col = db._create("overrideOneShardAndReplicationFactor", {replicationFactor: nonDefaultReplicationFactor});
          let colProperties = col.properties();

          if (isCluster) {
            assertEqual(colProperties.distributeShardsLike, "_graphs");
            assertEqual(colProperties.replicationFactor, db._properties().replicationFactor);
          }
        }
      }
    },
    
    testReplicationFactorAndOverrides : function () {
      assertTrue(db._createDatabase(dn, { replicationFactor : 2, writeConcern : 2 }));

      db._useDatabase(dn);
      let props = db._properties();
      if (!isCluster) {
        assertEqual(props.sharding, undefined);
        assertEqual(props.replicationFactor, undefined);
        assertEqual(props.minReplicationFactor, undefined);  // deprecated
        assertEqual(props.writeConcern, undefined);
      } else {
        assertEqual(props.sharding, "");
        assertEqual(props.replicationFactor, 2);
        assertEqual(props.writeConcern, 2);

        {
          let col = db._create("somecollection");
          let colProperties = col.properties();
          assertEqual(colProperties.replicationFactor, 2);
          assertEqual(colProperties.minReplicationFactor, 2);  // deprecated
          assertEqual(colProperties.writeConcern, 2);
        }

        {
          let col = db._create("overrideCollection1", { writeConcern : 1});
          let colProperties = col.properties();
          assertEqual(colProperties.minReplicationFactor, 1);  // deprecated
          assertEqual(colProperties.writeConcern, 1);
        }

        {
          let col = db._create("overrideCollection2", { writeConcern : 1, replicationFactor : 1, numberOfShards : 3});
          let colProperties = col.properties();
          assertEqual(colProperties.minReplicationFactor, 1);  // deprecated
          assertEqual(colProperties.writeConcern, 1);
        }

        {
          // we want to create a normal collection and have a different replication factor
          let col2 = db._collection("overrideCollection2");
          try {
            // Write concern is not inherited, the replicationFactor is. The default for the database is 2
            // we inherit replication factor 1 by distribute shards like, with default wc 2. This is forbidden,
            // as writeConcern is higher than replicationFactor.
            db._create("overrideCollection3", { distributeShardsLike: "overrideCollection2"});
            fail();
          } catch (err) {
            assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
          }
          // Giving correct write concern we can do distributeShardsLike properly
          let col3 = db._create("overrideCollection3", { distributeShardsLike: "overrideCollection2", writeConcern: 1});
          let col2Properties = col2.properties();
          let col3Properties = col3.properties();

          assertEqual(col3Properties.distributeShardsLike, col2.name());
          assertEqual(col3Properties.replicationFactor, col2Properties.replicationFactor);
        }
      }
    },

    testSatelliteDB : function () {
      assertTrue(db._createDatabase(dn, { replicationFactor : "satellite"}));
      db._useDatabase(dn);
      let props = db._properties();
      if (!isCluster) {
        assertEqual(props.sharding, undefined);
      } else  {
        assertEqual(props.sharding, "");
        if (isEnterprise) {
          assertEqual(props.replicationFactor, "satellite");
        } else {
          //without enterprise we can not have a replication factor of 1
          assertEqual(props.replicationFactor, defaultReplicationFactor);
        }
      }
    },
    
    testValuesBelowMinReplicationFactor : function () {
      let min = internal.minReplicationFactor;
      if (min > 0) {
        try {
          db._createDatabase(dn, { replicationFactor : min - 1 });
        } catch (err) {
          assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
        }
      }
    },

    testValuesAboveMaxReplicationFactor : function () {
      let max = internal.maxReplicationFactor;
      if (max > 0) {
        try {
          db._createDatabase(dn, { replicationFactor : max + 1 });
        } catch (err) {
          assertEqual(ERRORS.ERROR_BAD_PARAMETER.code, err.errorNum);
        }
      }
    },
  
  };
}

jsunity.run(OneShardPropertiesSuite);
return jsunity.done();
