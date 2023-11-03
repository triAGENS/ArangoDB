/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global fail, assertEqual, assertNotEqual, assertTrue */


////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2023 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Valery Mironov
/// @author Copyright 2023, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const db = require("@arangodb").db;
const _ = require('lodash');

const ViewBitsetJoin = function () {
  const databaseName = "BitsetJoinDatabase";
  const collectionName1 = "BitsetJoinCollection1";
  const viewName1 = "BitsetJoinView1";
  const collectionName2 = "BitsetJoinCollection2";
  const viewName2 = "BitsetJoinView2";

  const checkQuery = function (query, immutableParts) {
    const statement1 = db._createStatement({query: query, options: {
      optimizer: {
        rules: ["-immutable-search-condition"]
      }
    }});
    const statement2 = db._createStatement({query: query, options: {}});
    const plan1 = statement1.explain().plan;
    const plan2 = statement2.explain().plan;
    let node1 = plan1.nodes.find(node => node.view === viewName2);
    let node2 = plan2.nodes.find(node => node.view === viewName2);
    assertTrue(node1.immutableParts === undefined);
    assertTrue(node2.immutableParts === immutableParts);
    const result1 = statement1.execute();
    const result2 = statement2.execute();
    assertEqual(result1.toArray(), result2.toArray());
  };

  return {
    setUpAll: function () {
      db._createDatabase(databaseName);
      db._useDatabase(databaseName); 

      let c1 = db._create(collectionName1);
      let a = [];
      for (let i = 0; i != 1000; ++i) {
        a.push({
          "id": i * 3,
        });
      }
      c1.insert(a);
      db._createView(viewName1, "arangosearch", {
        "links": {
          [collectionName1]: {
            includeAllFields: true
          }
        }
      });

      let c2 = db._create(collectionName2);
      for (let i = 0; i != 1000; ++i) {
        a.push({
          "id": i * 2,
          "term": i,
        });
      }
      c2.insert(a);
      db._createView(viewName2, "arangosearch", {
        "links": {
          [collectionName2]: {
            includeAllFields: true
          }
        }
      });
    },

    tearDownAll: function () {
      db._useDatabase("_system");
      db._dropDatabase(databaseName);
    },

    testJoinViewAnd: function () {
      const query = "FOR d1 IN " + viewName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id AND d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 2);
    },

    testJoinCollectionAnd: function () {
      const query = "FOR d1 IN " + collectionName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id AND d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 2);
    },

    testJoinViewOr: function () {
      const query = "FOR d1 IN " + viewName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id OR d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 2);
    },

    testJoinCollectionOr: function () {
      const query = "FOR d1 IN " + collectionName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id OR d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 2);
    },

    testJoinViewAll: function () {
      const query = "FOR d1 IN " + viewName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 0);
    },

    testJoinCollectionAll: function () {
      const query = "FOR d1 IN " + collectionName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d2.term == 3 RETURN [d1, d2]";
      checkQuery(query, 0);
    },

    testJoinViewNone: function () {
      const query = "FOR d1 IN " + viewName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id RETURN [d1, d2]";
      checkQuery(query, undefined);
    },

    testJoinCollectionNone: function () {
      const query = "FOR d1 IN " + collectionName1 + " FOR d2 IN " + viewName2 + 
        " SEARCH d1.id == d2.id RETURN [d1, d2]";
      checkQuery(query, undefined);
    },
  };
};

jsunity.run(ViewBitsetJoin);
return jsunity.done();
