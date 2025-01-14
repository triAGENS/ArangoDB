/*global assertEqual, assertTrue, assertFalse, print */
"use strict";
////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2015-2025 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
/// @author Max Neunhoeffer
/// @copyright Copyright 2025, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////
const jsunity = require("jsunity");
const {db} = require("@arangodb");

const database = "myGeoDatabase";
const collection = "myColl";

function testGeoCluster() {
  return {
    setUpAll: function () {
      db._createDatabase(database);
      db._useDatabase(database);
      db._create(collection, {numberOfShards:3});
      db._collection(collection).ensureIndex(
        {type: "geo", fields: [`geo`], geoJson: true});
    },
    tearDownAll: function () {
      db._useDatabase("_system");
      db._dropDatabase(database);
    },

    testSortNodeGone: function () {
      // For some time we had the bug that in the cluster, a sort node 
      // was not removed in the execution plan in case of a geo query.
      // The sort node is not needed, since the geo index delivers sorted
      // results anyway. Its present is bad for performance, since it leads
      // to the fact that the query execution reads **all** documents in 
      // the collection from the geo index. Therefore we check that the
      // sort node is removed during query optimization here:
      let plan = db._createStatement(`
        FOR d IN ${collection}
          SORT GEO_DISTANCE(d.geo, [0,0])
          RETURN d
      `).explain();
      let nodetypes = plan.plan.nodes.map(x => x.type);
      assertEqual(-1, nodetypes.indexOf("SortNode"));
    }
  };
}

jsunity.run(testGeoCluster);
return jsunity.done();
