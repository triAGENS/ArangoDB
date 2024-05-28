/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, assertNotEqual */

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
/// @author Copyright 2020, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const db = require("@arangodb").db;
const graphHelper = require("@arangodb/graph/helpers");
const isCluster = require("internal").isCluster();
const isEnterprise = require("internal").isEnterprise();

function edgeCollectionRestrictionSuite() {
  const vn = "UnitTestsVertex";
  const en = "UnitTestsEdges";
  const gn = "UnitTestsGraph";

  return {

    testBasic : function () {
      const graphs = require("@arangodb/general-graph");
      let cleanup = function() {
        try {
          graphs._drop(gn, true);
        } catch (err) {}
        db._drop(vn);
        db._drop(en + "1");
        db._drop(en + "2");
      };
      cleanup();

      try {
        db._create(vn, { numberOfShards: 4 });
        db._createEdgeCollection(en + "1", { numberOfShards: 4 });
        db._createEdgeCollection(en + "2", { numberOfShards: 4 });

        graphs._create(gn, [graphs._relation(en + "1", vn, vn), graphs._relation(en + "2", vn, vn)]);

        graphHelper.runTraversalRestrictEdgeCollectionTests(vn, en, gn);
      } finally {
        cleanup();
      }
    },

    testOneShard : function () {
      if (!isEnterprise || !isCluster) {
        return;
      }

      const graphs = require("@arangodb/general-graph");
      let cleanup = function() {
        try {
          graphs._drop(gn, true);
        } catch (err) {}
        db._drop(vn);
        db._drop(en + "1");
        db._drop(en + "2");
      };
      cleanup();
   
      try {
        db._create(vn, { numberOfShards: 1 });
        db._createEdgeCollection(en + "1", { distributeShardsLike: vn, numberOfShards: 1 });
        db._createEdgeCollection(en + "2", { distributeShardsLike: vn, numberOfShards: 1 });

        graphs._create(gn, [graphs._relation(en + "1", vn, vn), graphs._relation(en + "2", vn, vn)]);

        graphHelper.runTraversalRestrictEdgeCollectionTests(vn, en, gn, "cluster-one-shard");
      } finally {
        cleanup();
      }
    },

    testSmart : function () {
      if (!isEnterprise || !isCluster) {
        return;
      }

      const graphs = require("@arangodb/smart-graph");
      let cleanup = function() {
        try {
          graphs._drop(gn, true);
        } catch (err) {}
        db._drop(vn);
        db._drop(en + "1");
        db._drop(en + "2");
      };
      cleanup();
   
      try {
        const smartGraphAttribute = 'smart';
        graphs._create(gn, [graphs._relation(en + "1", vn, vn), graphs._relation(en + "2", vn, vn)], null, { numberOfShards: 4, smartGraphAttribute: smartGraphAttribute });
        graphHelper.runTraversalRestrictEdgeCollectionTests(vn, en, gn, false, smartGraphAttribute);
      } finally {
        cleanup();
      }
    },
  };
}

jsunity.run(edgeCollectionRestrictionSuite);

return jsunity.done();
