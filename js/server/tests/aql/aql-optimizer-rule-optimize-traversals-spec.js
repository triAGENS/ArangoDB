/* jshint globalstrict:false, strict:false, maxlen: 5000 */
/* global describe, before, after, it, AQL_EXPLAIN, AQL_EXECUTE, AQL_EXECUTEJSON */
'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / @brief tests for optimizer rules
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License");
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
// / @author Wilfried Goesgens, Michael Hackstein
// / @author Copyright 2014, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////
//
const expect = require('chai').expect;

const helper = require('@arangodb/aql-helper');
const isEqual = helper.isEqual;
const graphModule = require('@arangodb/general-graph');
const graphName = 'myUnittestGraph';

let graph;
let edgeKey;

const ruleName = 'optimize-traversals';
// various choices to control the optimizer:
const paramEnabled = { optimizer: { rules: [ '-all', '+' + ruleName ] } };
const paramDisabled = { optimizer: { rules: [ '+all', '-' + ruleName ] } };

const cleanup = () => {
  try {
    graphModule._drop(graphName, true);
  } catch (x) {
  }
};
// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

describe('Rule optimize-traversals', () => {
  // / @brief set up
  before(() => {
    cleanup();
    graph = graphModule._create(graphName, [
      graphModule._relation('edges', 'circles', 'circles')]);

    // Add circle circles
    graph.circles.save({'_key': 'A', 'label': '1'});
    graph.circles.save({'_key': 'B', 'label': '2'});
    graph.circles.save({'_key': 'C', 'label': '3'});
    graph.circles.save({'_key': 'D', 'label': '4'});
    graph.circles.save({'_key': 'E', 'label': '5'});
    graph.circles.save({'_key': 'F', 'label': '6'});
    graph.circles.save({'_key': 'G', 'label': '7'});

    // Add relevant edges
    edgeKey = graph.edges.save('circles/A', 'circles/C', {theFalse: false, theTruth: true, 'label': 'foo'})._key;
    graph.edges.save('circles/A', 'circles/B', {theFalse: false, theTruth: true, 'label': 'bar'});
    graph.edges.save('circles/B', 'circles/D', {theFalse: false, theTruth: true, 'label': 'blarg'});
    graph.edges.save('circles/B', 'circles/E', {theFalse: false, theTruth: true, 'label': 'blub'});
    graph.edges.save('circles/C', 'circles/F', {theFalse: false, theTruth: true, 'label': 'schubi'});
    graph.edges.save('circles/C', 'circles/G', {theFalse: false, theTruth: true, 'label': 'doo'});
  });

  // / @brief tear down
  after(cleanup);

  it('should remove unused variables', () => {
    const queries = [
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN 1`, true, [ true, false, false ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v, e]`, true, [ true, true, false ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v, p]`, true, [ true, false, true ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [e, p]`, false, [ true, true, true ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v]`, true, [ true, false, false ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [e]`, true, [ true, true, false ] ],
      [ `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [p]`, true, [ true, false, true ] ],
      [ `FOR v, e IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v, e]`, false, [ true, true, false ] ],
      [ `FOR v, e IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v]`, true, [ true, false, false ] ],
      [ `FOR v IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' RETURN [v]`, false, [ true, false, false ] ]
    ];

    queries.forEach((query) => {
      const result = AQL_EXPLAIN(query[0], { }, paramEnabled);
      expect(result.plan.rules.indexOf(ruleName) !== -1).to.equal(query[1], query);

      // check that variables were correctly optimized away
      let found = false;
      result.plan.nodes.forEach(function (thisNode) {
        if (thisNode.type === 'TraversalNode') {
          expect(thisNode.hasOwnProperty('vertexOutVariable')).to.equal(query[2][0]);
          expect(thisNode.hasOwnProperty('edgeOutVariable')).to.equal(query[2][1]);
          expect(thisNode.hasOwnProperty('pathOutVariable')).to.equal(query[2][2]);
          found = true;
        }
      });
      expect(found).to.be.true;
    });
  });

  it('should not take effect in these queries', () => {
    const queries = [
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      LET localScopeVar = NOOPT(true) FILTER p.edges[0].theTruth == localScopeVar
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[-1].theTruth == true
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[*].theTruth == true or p.edges[1].label == 'bar'
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[RAND()].theFalse == false
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[p.edges.length - 1].theFalse == false
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 2 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER CONCAT(p.edges[0]._key, '') == " + edgeKey + " SORT v._key
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 2 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER NOOPT(CONCAT(p.edges[0]._key, '')) == " + edgeKey + " SORT v._key
      RETURN {v:v,e:e,p:p}`,
      `FOR v, e, p IN 2 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER NOOPT(V8(CONCAT(p.edges[0]._key, ''))) == " + edgeKey + " SORT v._key
      RETURN {v:v,e:e,p:p}`,
      `FOR snippet IN ['a', 'b']
      FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[1].label == CONCAT(snippet, 'ar')
      RETURN {v,e,p}`
    ];

    queries.forEach(function (query) {
      const result = AQL_EXPLAIN(query, { }, paramEnabled);
      expect(result.plan.rules.indexOf(ruleName)).to.equal(-1, query);
    });
  });

  it('should take effect in these queries', () => {
    const queries = [
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].theTruth == true
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[1].theTruth == true
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[2].theTruth == true
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[2].theTruth == true AND    p.edges[1].label == 'bar'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].theTruth == true FILTER p.edges[1].label == 'bar'
      RETURN {v,e,p}`,
      `FOR snippet IN ['a', 'b']
      LET test = CONCAT(snippet, 'ar')
      FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[1].label == test 
      RETURN {v,e,p}`
    ];
    queries.forEach(function (query) {
      const result = AQL_EXPLAIN(query, { }, paramEnabled);
      expect(result.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);
      result.plan.nodes.forEach((thisNode) => {
        if (thisNode.type === 'TraversalNode') {
          expect(thisNode.hasOwnProperty('condition')).to.equal(true, query);
        }
      });
    });
  });

  it('should not modify the result', () => {
    var queries = [
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].label == 'foo'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[1].label == 'foo'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].theTruth == true FILTER p.edges[0].label == 'foo'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].theTruth == true FILTER p.edges[1].label == 'foo'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[0].theTruth == true FILTER p.edges[0].label == 'foo'
      RETURN {v,e,p}`,
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}'
      FILTER p.edges[1].theTruth == true FILTER p.edges[1].label == 'foo'
      RETURN {v,e,p}`
    ];
    const opts = { allPlans: true, verbosePlans: true, optimizer: { rules: [ '-all', '+' + ruleName ] } };

    queries.forEach(function (query) {
      const planDisabled = AQL_EXPLAIN(query, { }, paramDisabled);
      const planEnabled = AQL_EXPLAIN(query, { }, paramEnabled);
      const resultDisabled = AQL_EXECUTE(query, { }, paramDisabled).json;
      const resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;

      expect(isEqual(resultDisabled, resultEnabled)).to.equal(true, query);

      expect(planDisabled.plan.rules.indexOf(ruleName)).to.equal(-1, query);
      expect(planEnabled.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);

      const plans = AQL_EXPLAIN(query, {}, opts).plans;
      plans.forEach(function (plan) {
        const jsonResult = AQL_EXECUTEJSON(plan, { optimizer: { rules: [ '-all' ] } }).json;
        expect(jsonResult).to.deep.equal(resultDisabled, query);
      });
    });
  });

  it('should prune when using functions', () => {
    let queries = [
      `WITH circles FOR v, e, p IN 2 OUTBOUND @start @@ecol
      FILTER p.edges[0]._key == CONCAT(@edgeKey, '')
      SORT v._key RETURN v._key`,
      `WITH circles FOR v, e, p IN 2 OUTBOUND @start @@ecol
      FILTER p.edges[0]._key == @edgeKey
      SORT v._key RETURN v._key`,
      `WITH circles FOR v, e, p IN 2 OUTBOUND @start @@ecol
      FILTER CONCAT(p.edges[0]._key, '') == @edgeKey
      SORT v._key RETURN v._key`,
      `WITH circles FOR v, e, p IN 2 OUTBOUND @start @@ecol
      FILTER NOOPT(CONCAT(p.edges[0]._key, '')) == @edgeKey
      SORT v._key RETURN v._key`,
      `WITH circles FOR v, e, p IN 2 OUTBOUND @start @@ecol
      FILTER NOOPT(V8(CONCAT(p.edges[0]._key, ''))) == @edgeKey
      SORT v._key RETURN v._key`
    ];
    const bindVars = {
      start: 'circles/A',
      '@ecol': 'edges',
      edgeKey: edgeKey
    };
    queries.forEach(function (q) {
      let res = AQL_EXECUTE(q, bindVars, paramDisabled).json;
      expect(res.length).to.equal(2, 'query: ' + q);
      expect(res[0]).to.equal('F', 'query: ' + q);
      expect(res[1]).to.equal('G', 'query: ' + q);

      res = AQL_EXECUTE(q, bindVars, paramEnabled).json;

      expect(res.length).to.equal(2, 'query (enabled): ' + q);
      expect(res[0]).to.equal('F', 'query (enabled): ' + q);
      expect(res[1]).to.equal('G', 'query (enabled): ' + q);
    });
  });

  it('should short-circuit conditions that cannot be fulfilled', () => {
    let queries = [
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' FILTER p.edges[7].label == 'foo' RETURN {v,e,p}`,
      // indexed access starts with 0 - this is also forbidden since it will look for the 6th!
      `FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH '${graphName}' FILTER p.edges[5].label == 'foo' RETURN {v,e,p}`
      // "FOR v, e, p IN 1..5 OUTBOUND 'circles/A' GRAPH 'myGraph' FILTER p.edges[1].label == 'foo' AND p.edges[1].label == 'bar' return {v:v,e:e,p:p}"
    ];

    queries.forEach(function (query) {
      let result = AQL_EXPLAIN(query, { }, paramEnabled);
      let simplePlan = helper.getCompactPlan(result);
      expect(result.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);
      expect(simplePlan[2].type).to.equal('NoResultsNode');
    });
  });

  describe('using condition vars', () => {
    it('taking invalid start form subquery', () => {
      const query = `
        LET data = (FOR i IN 1..1 RETURN i)
        FOR v, e, p IN 1..10 OUTBOUND data GRAPH '${graphName}'
        FILTER p.vertices[0]._id == '123'
        FILTER p.vertices[1]._id != null
        FILTER p.edges[0]._id IN data[*].foo.bar
        RETURN 1`;

      const result = AQL_EXPLAIN(query);
      expect(result.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);
      expect(AQL_EXECUTE(query).json.length).to.equal(0);
    });

    it('filtering on a subquery', () => {
      const query = `
        LET data = (FOR i IN 1..1 RETURN i)
        FOR v, e, p IN 1..10 OUTBOUND 'circles/A' GRAPH '${graphName}'
        FILTER p.vertices[0]._id == '123'
        FILTER p.vertices[1]._id != null
        FILTER p.edges[0]._id IN data[*].foo.bar
        RETURN 1`;

      const result = AQL_EXPLAIN(query);
      expect(result.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);
      expect(AQL_EXECUTE(query).json.length).to.equal(0);
    });

    it('filtering on a subquery 2', () => {
      const query = `
        LET data = (FOR i IN 1..1 RETURN i)
        FOR v, e, p IN 1..10 OUTBOUND 'circles/A' GRAPH '${graphName}'
        FILTER p.vertices[0]._id == '123'
        FILTER p.vertices[1]._id != null
        FILTER p.edges[0]._id IN data[*].foo.bar
        FILTER p.edges[1]._key IN data[*].bar.baz._id
        RETURN 1`;

      const result = AQL_EXPLAIN(query);
      expect(result.plan.rules.indexOf(ruleName)).to.not.equal(-1, query);
      expect(AQL_EXECUTE(query).json.length).to.equal(0);
    });
  });

  describe('filtering on own output', () => {
    it('should filter with v on path', () => {
      let query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER v.bar == p.edges[0].foo
        RETURN {v,e,p}
      `;
      const bindVars = {
        start: 'circles/A',
        graph: graphName
      };

      let plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      let result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);

      query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.edges[0].foo == v.bar 
        RETURN {v,e,p}
      `;

      plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);
    });

    it('should filter with e on path', () => {
      let query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER e.bar == p.edges[0].foo
        RETURN {v,e,p}
      `;
      const bindVars = {
        start: 'circles/A',
        graph: graphName
      };
      let plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      let result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);

      query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.edges[0].foo == e.bar 
        RETURN {v,e,p}
      `;

      plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);
    });

    it('should filter with path.edges on path.edges', () => {
      let query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.edges[1].foo == p.edges[0].foo
        RETURN {v,e,p}
      `;
      const bindVars = {
        start: 'circles/A',
        graph: graphName
      };

      let plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      let result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);

      query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.edges[0].foo == p.edges[1].foo
        RETURN {v,e,p}
      `;

      result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);
    });

    it('should filter with path.vertices on path.vertices', () => {
      let query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.vertices[1].foo == p.vertices[0].foo
        RETURN {v,e,p}
      `;
      const bindVars = {
        start: 'circles/A',
        graph: graphName
      };
      let plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      let result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);

      query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.vertices[0].foo == p.vertices[1].foo
        RETURN {v,e,p}
      `;

      plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);
    });

    it('should filter with path.vertices on path.edges', () => {
      let query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.vertices[1].foo == p.edges[0].foo
        RETURN {v,e,p}
      `;
      const bindVars = {
        start: 'circles/A',
        graph: graphName
      };

      let plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      let result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);

      query = `
        FOR v,e,p IN 2 OUTBOUND @start GRAPH @graph
        FILTER p.edges[0].foo == p.vertices[1].foo
        RETURN {v,e,p}
      `;

      plan = AQL_EXPLAIN(query, bindVars, paramEnabled);
      expect(plan.plan.rules.indexOf(ruleName)).to.equal(-1, query);

      result = AQL_EXECUTE(query, bindVars).json;
      expect(result.length).to.equal(4);
    });
  });
});
