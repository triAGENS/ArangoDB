/*jshint globalstrict:true, strict:true, esnext: true */

"use strict";

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

const internal = require("internal");
const db = internal.db;
const sgm = require("@arangodb/smart-graph");
const cgm = require("@arangodb/general-graph");
const _ = require("lodash");


const TestVariants = Object.freeze({
  SingleServer: 1,
  GeneralGraph: 2,
  SmartGraph: 3,
});

class TestGraph {
  constructor (graphName, edges, eRel, vn, en, protoSmartSharding, testVariant, numberOfShards) {
    this.graphName = graphName;
    this.edges = edges;
    this.eRel = eRel;
    this.vn = vn;
    this.en = en;
    this.protoSmartSharding = protoSmartSharding;
    this.testVariant = testVariant;
    this.numberOfShards = numberOfShards;
  }

  create() {
    switch (this.testVariant) {
      case TestVariants.SingleServer: {
        cgm._create(this.name(), [this.eRel], [], {});
        break;
      }
      case TestVariants.GeneralGraph: {
        const options = {numberOfShards: this.numberOfShards};
        cgm._create(this.name(), [this.eRel], [], options);
        break;
      }
      case TestVariants.SmartGraph: {
        const options = {
          numberOfShards: this.numberOfShards,
          smartGraphAttribute: ProtoGraph.smartAttr(),
          isSmart: true
        };
        sgm._create(this.name(), [this.eRel], [], options);
        break;
      }
    }

    if (this.testVariant === TestVariants.SingleServer) {
      this.verticesByName = TestGraph._fillGraph(this.graphName, this.edges, db[this.vn], db[this.en]);
    } else {
      const shardAttrsByShardIndex = this._shardAttrPerShard(db[this.vn]);
      const vertexSharding = this.protoSmartSharding.map(([v, i]) => [v, shardAttrsByShardIndex[i]]);
      this.verticesByName = TestGraph._fillGraph(this.graphName, this.edges, db[this.vn], db[this.en], vertexSharding);
    }
  }

  name() {
    return this.graphName;
  }

  vertex(name) {
    return this.verticesByName[name];
  }

  drop() {
    cgm._drop(this.name(), true);
  }

  /**
   * @param gn Graph Name
   * @param edges Array of pairs of strings, e.g. [["A", "B"], ["B", "C"]]
   * @param vc Vertex collection
   * @param ec Edge collection
   * @param vertexSharding Array of pairs, where the first element is the vertex
   *                       key and the second the smart attribute.
   * @private
   */
  static _fillGraph(gn, edges, vc, ec, vertexSharding = []) {
    const vertices = new Map(vertexSharding);
    for (const edge of edges) {
      if (!vertices.has(edge[0])) {
        vertices.set(edge[0], null);
      }
      if (!vertices.has(edge[1])) {
        vertices.set(edge[1], null);
      }
    }

    const verticesByName = {};
    for (const [vertexKey, smart] of vertices) {
      const doc = {key: vertexKey};
      if (smart !== null) {
        doc[ProtoGraph.smartAttr()] = smart;
      }
      verticesByName[vertexKey] = vc.save(doc)._id;
    }
    for (const edge of edges) {
      let v = verticesByName[edge[0]];
      let w = verticesByName[edge[1]];
      ec.save(v, w, {});
    }
    return verticesByName;
  }

  _shardAttrPerShard(col) {
    const shards = col.shards();

    // Create an array of size numberOfShards, each entry null.
    const exampleAttributeByShard = _.fromPairs(shards.map(id => [id, null]));

    const done = () => !
      _.values(exampleAttributeByShard)
      .includes(null);
    let i = 0;
    // const key = this.enterprise ? ProtoGraph.smartAttr() : "_key";
    const key = "_key";
    while (!done()) {
      const value = this.enterprise ? i.toString() + ":" : i.toString() ;
      const doc = {[key]: value};

      let shard;
      try {
        shard = col.getResponsibleShard(doc);
      } catch (e) {
        throw new Error('Tried to get shard for ' + JSON.stringify(doc) + ', original error: ' + e);
      }
      if (exampleAttributeByShard[shard] === null) {
        exampleAttributeByShard[shard] = value;
      }

      ++i;
    }

    return _.values(exampleAttributeByShard);
  }
}

class ProtoGraph {
  static smartAttr() { return "smart"; }

  constructor (name, edges, generalShardings, smartShardings) {
    this.protoGraphName = name;
    this.edges = edges;
    this.generalShardings = generalShardings;
    this.smartShardings = smartShardings;
  }

  name() {
    return this.protoGraphName;
  }

  prepareSingleServerGraph() {
    const vn = this.protoGraphName + '_Vertex';
    const en = this.protoGraphName + '_Edge';
    const gn = this.protoGraphName + '_Graph';
    const eRel = cgm._relation(en, vn, vn);

    return [new TestGraph(gn, this.edges, eRel, vn, en, [], TestVariants.SingleServer)];
  }

  prepareGeneralGraphs() {
    return this.generalShardings.map(numberOfShards =>  {
      const suffix = `_${numberOfShards}shards`;
      const vn = this.protoGraphName + '_Vertex' + suffix;
      const en = this.protoGraphName + '_Edge' + suffix;
      const gn = this.protoGraphName + '_Graph' + suffix;

      const eRel = cgm._relation(en, vn, vn);

      return new TestGraph(gn, this.edges, eRel, vn, en, [], TestVariants.GeneralGraph, numberOfShards);
    });
  }

  prepareSmartGraphs() {
    return this.smartShardings.map((sharding) =>  {
      const {numberOfShards, vertexSharding} = sharding;
      const suffix = ProtoGraph._buildSmartSuffix(sharding);

      const vn = this.protoGraphName + '_Vertex' + suffix;
      const en = this.protoGraphName + '_Edge' + suffix;
      const gn = this.protoGraphName + '_Graph' + suffix;

      const eRel = sgm._relation(en, vn, vn);

      return new TestGraph(gn, this.edges, eRel, vn, en, vertexSharding, TestVariants.SmartGraph, numberOfShards);
    });
  }

  static _buildSmartSuffix({numberOfShards, vertexSharding}) {
    // vertexSharding is an array of pairs, each pair holding a vertex (string)
    // and a shard (index), e.g. [["A", 0], ["B", 0], ["C", 1]].
    // For this (with 2 shards) this will return the string
    //   "_2shards_s0-AB_s1-C"

    return `_${numberOfShards}shards_` +
      _.toPairs(
        _.groupBy(vertexSharding, ([,s]) => s)
      ).map(
        ([s, vs]) => 's' + s + '-' + vs.map(([v,]) => v).join('')
      ).join('_');
  }


}

const protoGraphs = {

  /*
   *       B       E
   *     ↗   ↘   ↗
   *   A       D
   *     ↘   ↗   ↘
   *       C       F
   */
  openDiamond: new ProtoGraph("openDiamond", [
      ["A", "B"],
      ["A", "C"],
      ["B", "D"],
      ["C", "D"],
      ["D", "E"],
      ["D", "F"],
    ],
    [1, 2, 5],
    [
      {
        numberOfShards: 1,
        vertexSharding:
          [
            ["A", 0],
            ["B", 0],
            ["C", 0],
            ["D", 0],
            ["E", 0],
            ["F", 0],
          ],
      },
      {
        numberOfShards: 2,
        vertexSharding:
          [
            ["A", 0],
            ["B", 1],
            ["C", 0],
            ["D", 0],
            ["E", 0],
            ["F", 0],
          ],
      },
      {
        numberOfShards: 2,
        vertexSharding:
          [
            ["A", 0],
            ["B", 0],
            ["C", 0],
            ["D", 1],
            ["E", 0],
            ["F", 0],
          ],
      },
      {
        numberOfShards: 2,
        vertexSharding:
          [
            ["A", 0],
            ["B", 0],
            ["C", 0],
            ["D", 0],
            ["E", 1],
            ["F", 1],
          ],
      },
      {
        numberOfShards: 6,
        vertexSharding:
          [
            ["A", 0],
            ["B", 1],
            ["C", 2],
            ["D", 3],
            ["E", 4],
            ["F", 5],
          ],
      },
    ]
  ),
};

exports.ProtoGraph = ProtoGraph;
exports.protoGraphs = protoGraphs;
