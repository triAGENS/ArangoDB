/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, AQL_EXECUTE, assertTrue, fail */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for regression returning blocks to the manager
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer
/// @author Copyright 2020, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var internal = require("internal");
var errors = internal.errors;
var db = require("@arangodb").db,
  indexId;

// This example was produced by Jan Steeman to reproduce a
// crash in the TraversalExecutor code
const productCollectionName = "RegressionProduct";
const customerCollectionName = "RegressionCustomer";
const ownsEdgeCollectionName = "RegressionOwns";

const expectedResult = [
  {
    _key: "396",
    _id: customerCollectionName + "/396",
    name: "John",
    surname: "Smith",
    age: 52,
    alive: false,
    _class: "com.arangodb.springframework.testdata.Customer"
  },
  {
    _key: "397",
    _id: customerCollectionName + "/397",
    name: "Matt",
    surname: "Smith",
    age: 34,
    alive: false,
    _class: "com.arangodb.springframework.testdata.Customer"
  }
];

var cleanup = function() {
  db._drop(productCollectionName);
  db._drop(customerCollectionName);
  db._drop(ownsEdgeCollectionName);
};

var createBaseGraph = function() {
  db._create(customerCollectionName, {
    cacheEnabled: false,
    isSystem: false,
    keyOptions: { allowUserKeys: true, type: "traditional", lastValue: 0 },
    usesRevisionsAsDocumentIds: true,
    waitForSync: false,
    writeConcern: 1
  });

  db[customerCollectionName].ensureIndex({
    fields: ["location"],
    geoJson: false,
    name: "idx_1661521530578796544",
    sparse: true,
    type: "geo",
    unique: false
  });

  db._create(productCollectionName, {
    cacheEnabled: false,
    keyOptions: { allowUserKeys: true, type: "traditional", lastValue: 0 },
    waitForSync: false,
    writeConcern: 1
  });
  db[productCollectionName].ensureIndex({
    fields: ["location"],
    geoJson: false,
    name: "idx_1661521530657439744",
    sparse: true,
    type: "geo",
    unique: false
  });

  db._createEdgeCollection(ownsEdgeCollectionName, {
    cacheEnabled: false,
    keyOptions: { allowUserKeys: true, type: "traditional", lastValue: 0 },
    waitForSync: false,
    writeConcern: 1
  });

  db[customerCollectionName].insert([
    {
      _key: "396",
      _id: customerCollectionName + "/396",
      _rev: "_aM4ZTRW---",
      name: "John",
      surname: "Smith",
      age: 52,
      alive: false,
      _class: "com.arangodb.springframework.testdata.Customer"
    },
    {
      _key: "397",
      _id: customerCollectionName + "/397",
      _rev: "_aM4ZTRW--_",
      name: "Matt",
      surname: "Smith",
      age: 34,
      alive: false,
      _class: "com.arangodb.springframework.testdata.Customer"
    },
    {
      _key: "398",
      _id: customerCollectionName + "/398",
      _rev: "_aM4ZTRW--A",
      name: "Adam",
      surname: "Smith",
      age: 294,
      alive: false,
      _class: "com.arangodb.springframework.testdata.Customer"
    }
  ]);
  db[ownsEdgeCollectionName].insert([
    {
      _key: "400",
      _id: ownsEdgeCollectionName + "/400",
      _from: customerCollectionName + "/396",
      _to: productCollectionName + "/390",
      _rev: "_aM4ZTR2---",
      _class: "com.arangodb.springframework.testdata.Owns"
    },
    {
      _key: "402",
      _id: ownsEdgeCollectionName + "/402",
      _from: customerCollectionName + "/396",
      _to: productCollectionName + "/392",
      _rev: "_aM4ZTR6---",
      _class: "com.arangodb.springframework.testdata.Owns"
    },
    {
      _key: "404",
      _id: ownsEdgeCollectionName + "/404",
      _from: customerCollectionName + "/398",
      _to: productCollectionName + "/394",
      _rev: "_aM4ZTS----",
      _class: "com.arangodb.springframework.testdata.Owns"
    },
    {
      _key: "406",
      _id: ownsEdgeCollectionName + "/406",
      _from: customerCollectionName + "/397",
      _to: productCollectionName + "/390",
      _rev: "_aM4ZTSC---",
      _class: "com.arangodb.springframework.testdata.Owns"
    },
    {
      _key: "408",
      _id: ownsEdgeCollectionName + "/408",
      _from: customerCollectionName + "/397",
      _to: productCollectionName + "/392",
      _rev: "_aM4ZTSG---",
      _class: "com.arangodb.springframework.testdata.Owns"
    },
    {
      _key: "410",
      _id: ownsEdgeCollectionName + "/410",
      _from: customerCollectionName + "/397",
      _to: productCollectionName + "/394",
      _rev: "_aM4ZTSG--_",
      _class: "com.arangodb.springframework.testdata.Owns"
    }
  ]);
  db[productCollectionName].insert([
    {
      _key: "390",
      _id: productCollectionName + "/390",
      _rev: "_aM4ZTPy---",
      name: "phone",
      _class: "com.arangodb.springframework.testdata.Product"
    },
    {
      _key: "392",
      _id: productCollectionName + "/392",
      _rev: "_aM4ZTQG---",
      name: "car",
      _class: "com.arangodb.springframework.testdata.Product"
    },
    {
      _key: "394",
      _id: productCollectionName + "/394",
      _rev: "_aM4ZTQK---",
      name: "chair",
      _class: "com.arangodb.springframework.testdata.Product"
    }
  ]);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function traversalResetRegressionSuite() {
  return {
    ////////////////////////////////////////////////////////////////////////////////
    /// @brief set up
    ////////////////////////////////////////////////////////////////////////////////

    setUpAll: function() {
      cleanup();
      createBaseGraph();
    },

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief tear down
    ////////////////////////////////////////////////////////////////////////////////

    tearDownAll: function() {
      cleanup();
    },

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief test object access for path object
    ////////////////////////////////////////////////////////////////////////////////
    testTraversalResetCrashes: function() {
      const query = `WITH @@product
                       FOR e IN @@customer
                         FILTER (FOR e1 IN 1..1 ANY e._id @@owns
                                   FILTER e1.name == @0
                                   RETURN 1)[0] == 1 AND
                                (FOR e1 IN 1..1 ANY e._id @@owns
                                   FILTER e1.name == @1
                                   RETURN 1)[0] == 1
                         RETURN UNSET(e, "_rev")`;

      const bindVars = {
        "0": "phone",
        "1": "phone",
        "@product": productCollectionName,
        "@customer": customerCollectionName,
        "@owns": ownsEdgeCollectionName
      };

      var actual = db._query(query, bindVars);
      assertEqual(actual.toArray(), expectedResult);
    }
  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(traversalResetRegressionSuite);

return jsunity.done();
