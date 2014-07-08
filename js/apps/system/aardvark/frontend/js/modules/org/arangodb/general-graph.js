module.define("org/arangodb/general-graph", function(exports, module) {
/*jslint indent: 2, nomen: true, maxlen: 120, sloppy: true, vars: true, white: true, plusplus: true */
/*global require, exports, Graph, arguments */

////////////////////////////////////////////////////////////////////////////////
/// @brief Graph functionality
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
/// @author Florian Bartels, Michael Hackstein, Guido Schwab
/// @author Copyright 2011-2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////


var arangodb = require("org/arangodb"),
  internal = require("internal"),
  ArangoCollection = arangodb.ArangoCollection,
  ArangoError = arangodb.ArangoError,
  db = arangodb.db,
  errors = arangodb.errors,
  _ = require("underscore");


// -----------------------------------------------------------------------------
// --SECTION--                             module "org/arangodb/general-graph"
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief transform a string into an array.
////////////////////////////////////////////////////////////////////////////////


var stringToArray = function (x) {
  if (typeof x === "string") {
    return [x];
  }
  return _.clone(x);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if a parameter is not defined, an empty string or an empty
//  array
////////////////////////////////////////////////////////////////////////////////


var isValidCollectionsParameter = function (x) {
  if (!x) {
    return false;
  }
  if (Array.isArray(x) && x.length === 0) {
    return false;
  }
  if (typeof x !== "string" && !Array.isArray(x)) {
    return false;
  }
  return true;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief find or create a collection by name
////////////////////////////////////////////////////////////////////////////////

var findOrCreateCollectionByName = function (name, type, noCreate) {
  var col = db._collection(name),
    res = false;
  if (col === null && !noCreate) {
    if (type === ArangoCollection.TYPE_DOCUMENT) {
      col = db._create(name);
    } else {
      col = db._createEdgeCollection(name);
    }
    res = true;
  } else if (!(col instanceof ArangoCollection)) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NO_AN_ARANGO_COLLECTION.code;
    err.errorMessage = name + arangodb.errors.ERROR_GRAPH_NO_AN_ARANGO_COLLECTION.message;
    throw err;
  }
  return res;
};


////////////////////////////////////////////////////////////////////////////////
/// @brief find or create a collection by name
////////////////////////////////////////////////////////////////////////////////

var findOrCreateCollectionsByEdgeDefinitions = function (edgeDefinitions, noCreate) {
  var vertexCollections = {},
  edgeCollections = {};
  edgeDefinitions.forEach(function (e) {
    e.from.concat(e.to).forEach(function (v) {
      findOrCreateCollectionByName(v, ArangoCollection.TYPE_DOCUMENT, noCreate);
      vertexCollections[v] = db[v];
    });
    findOrCreateCollectionByName(e.collection, ArangoCollection.TYPE_EDGE, noCreate);
    edgeCollections[e.collection] = db[e.collection];
  });
  return [
    vertexCollections,
    edgeCollections
  ];
};

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function to get graphs collection
////////////////////////////////////////////////////////////////////////////////

var getGraphCollection = function() {
  var gCol = db._graphs;
  if (gCol === null || gCol === undefined) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NO_GRAPH_COLLECTION.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NO_GRAPH_COLLECTION.message;
    throw err;
  }
  return gCol;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function to print edge definitions in _PRINT
////////////////////////////////////////////////////////////////////////////////

var printEdgeDefinitions = function(defs) {
  return _.map(defs, function(d) {
    var out = d.collection;
    out += ": [";
    out += d.from.join(", ");
    out += "] -> [";
    out += d.to.join(", ");
    out += "]";
    return out;
  });
};

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function to wrap arango collections for overwrite
////////////////////////////////////////////////////////////////////////////////

var wrapCollection = function(col) {
  var wrapper = {};
  _.each(_.functions(col), function(func) {
    wrapper[func] = function() {
      return col[func].apply(col, arguments);
    };
  });
  return wrapper;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_example_description
/// 
/// For many of the following functions *examples* can be passed in as a parameter.
/// *Examples* are used to filter the result set for objects that match the conditions.
/// These *examples* can have the following values:
///
/// * *null*, there is no matching executed all found results are valid.
/// * A *string*, only results are returned, which *_id* equal the value of the string
/// * An example *object*, defining a set of attributes.
///     Only results having these attributes are matched.
/// * A *list* containing example *objects* and/or *strings*.
///     All results matching at least one of the elements in the list are returned.
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

var transformExample = function(example) {
  if (example === undefined) {
    return {};
  }
  if (typeof example === "string") {
    return {_id: example};
  }
  if (typeof example === "object") {
    if (Array.isArray(example)) {
      return _.map(example, function(e) {
        if (typeof e === "string") {
          return {_id: e};
        }
        return e;
      });
    }
    return example;
  }
  var err = new ArangoError();
  err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_EXAMPLE_ARRAY_OBJECT_STRING.code;
  err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_EXAMPLE_ARRAY_OBJECT_STRING.message;
  throw err;
};

var checkAllowsRestriction = function(colList, rest, msg) {
  var unknown = [];
  _.each(rest, function(r) {
    if (!colList[r]) {
      unknown.push(r);
    }
  });
  if (unknown.length > 0) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_BAD_PARAMETER.code;
    err.errorMessage = msg + ": "
    + unknown.join(" and ")
    + " are not known to the graph";
    throw err;
  }
  return true;
};


// -----------------------------------------------------------------------------
// --SECTION--                             module "org/arangodb/general-graph"
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                             Fluent AQL Interface
// -----------------------------------------------------------------------------

var AQLStatement = function(query, type) {
  this.query = query;
  if (type) {
    this.type = type;
  }
};

AQLStatement.prototype.printQuery = function() {
  return this.query;
};

AQLStatement.prototype.isPathQuery = function() {
  return this.type === "path";
};

AQLStatement.prototype.isPathVerticesQuery = function() {
  return this.type === "pathVertices";
};

AQLStatement.prototype.isPathEdgesQuery = function() {
  return this.type === "pathEdges";
};

AQLStatement.prototype.isEdgeQuery = function() {
  return this.type === "edge";
};

AQLStatement.prototype.isVertexQuery = function() {
  return this.type === "vertex";
};

AQLStatement.prototype.isNeighborQuery = function() {
  return this.type === "neighbor";
};

AQLStatement.prototype.allowsRestrict = function() {
  return this.isEdgeQuery()
    || this.isVertexQuery()
    || this.isNeighborQuery(); 
};

// -----------------------------------------------------------------------------
// --SECTION--                                AQL Generator for fluent interface
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief Starting point of the fluent interface.
///
/// Only for internal use.
////////////////////////////////////////////////////////////////////////////////

var AQLGenerator = function(graph) {
  this.stack = [];
  this.callStack = [];
  this.bindVars = {
    "graphName": graph.__name
  };
  this.graph = graph;
  this.cursor = null;
  this.lastVar = "";
  this._path = [];
  this._pathVertices = [];
  this._pathEdges = [];
  this._getPath = false;
};

AQLGenerator.prototype._addToPrint = function(name) {
  var args = Array.prototype.slice.call(arguments);
  args.shift(); // The Name
  var stackEntry = {};
  stackEntry.name = name;
  if (args.length > 0 && args[0] !== undefined) {
    stackEntry.params = args;
  } else {
    stackEntry.params = [];
  }
  this.callStack.push(stackEntry);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Print the call stack of this query
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._PRINT = function(context) {
  context.output = "[ GraphAQL ";
  context.output += this.graph.__name;
  _.each(this.callStack, function(call) {
    if(context.prettyPrint) {
      context.output += "\n";
    }
    context.output += ".";
    context.output += call.name;
    context.output += "(";
    var i = 0;
    for(i = 0; i < call.params.length; ++i) {
      if (i > 0) {
        context.output += ", ";
      }
      internal.printRecursive(call.params[i], context);
    }
    context.output += ")";
  });
  context.output += " ] ";
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Dispose and reset the current cursor of the query
///
/// Only for internal use.
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._clearCursor = function() {
  if (this.cursor) {
    this.cursor.dispose();
    this.cursor = null;
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Execute the query and keep the cursor
///
/// Only for internal use.
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._createCursor = function() {
  if (!this.cursor) {
    this.cursor = this.execute();
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief General edge query, takes direction as parameter
///
/// This will create the general AQL statement to load edges
/// connected to the vertices selected in the step before.
/// Will also bind the options into bindVars.
///
/// Only for internal use, user gets different functions for directions
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._edges = function(edgeExample, options) {
  this._clearCursor();
  this.options = options || {};
  var ex = transformExample(edgeExample);
  var edgeName = "edges_" + this.stack.length;
  var query = "FOR " + edgeName
    + ' IN GRAPH_EDGES(@graphName';
  if (!this.getLastVar()) {
    query += ',{}';
  } else {
    query += ',' + this.getLastVar();
  }
  query += ',@options_'
    + this.stack.length + ')';
  if (!Array.isArray(ex)) {
    ex = [ex];
  }
  this.options.edgeExamples = ex;
  this.bindVars["options_" + this.stack.length] = this.options;
  var stmt = new AQLStatement(query, "edge");
  this.stack.push(stmt);
  this.lastVar = edgeName;
  this._path.push(edgeName);
  this._pathEdges.push(edgeName);
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_edges
/// `graph_query.edges(examples)`
/// *Select all edges for the vertices selected before.*
///
/// Creates an AQL statement to select all edges for each of the vertices selected
/// in the step before.
/// This will include *inbound* as well as *outbound* edges.
/// The resulting set of edges can be filtered by defining one or more *examples*.
///
/// The complexity of this method is **O(n\*m^x)** with *n* being the vertices defined by the
/// parameter vertexExamplex, *m* the average amount of edges of a vertex and *x* the maximal depths.
/// Hence the default call would have a complexity of **O(n\*m)**;
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLEdgesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.edges().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered edges by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLEdgesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.edges({type: "married"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered edges by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLEdgesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.edges([{type: "married"}, {type: "friend"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.edges = function(example) {
  this._addToPrint("edges", example);
  return this._edges(example, {direction: "any"});
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_outEdges
/// `graph_query.outEdges(examples)`
/// *Select all outbound edges for the vertices selected before.*
/// 
///
/// Creates an AQL statement to select all *outbound* edges for each of the vertices selected
/// in the step before.
/// The resulting set of edges can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered outbound edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLOutEdgesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.outEdges().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered outbound edges by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLOutEdgesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.outEdges({type: "married"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered outbound edges by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLOutEdgesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.outEdges([{type: "married"}, {type: "friend"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.outEdges = function(example) {
  this._addToPrint("outEdges", example);
  return this._edges(example, {direction: "outbound"});
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_inEdges
/// `graph_query.inEdges(examples)`
/// *Select all inbound edges for the vertices selected before.*
/// 
///
/// Creates an AQL statement to select all *inbound* edges for each of the vertices selected
/// in the step before.
/// The resulting set of edges can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered inbound edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLInEdgesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.inEdges().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered inbound edges by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLInEdgesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.inEdges({type: "married"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered inbound edges by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLInEdgesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices([{name: "Alice"}, {name: "Bob"}]);
///   query.inEdges([{type: "married"}, {type: "friend"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.inEdges = function(example) {
  this._addToPrint("inEdges", example);
  return this._edges(example, {direction: "inbound"});
};

////////////////////////////////////////////////////////////////////////////////
/// @brief General vertex query, takes direction as parameter
///
/// This will create the general AQL statement to load vertices
/// connected to the edges selected in the step before.
/// Will also bind the options into bindVars.
///
/// Only for internal use, user gets different functions for directions
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._vertices = function(example, options) {
  this._clearCursor();
  this.options = options || {};
  var ex = transformExample(example);
  var vertexName = "vertices_" + this.stack.length;
  var query = "FOR " + vertexName
    + " IN GRAPH_VERTICES(@graphName,@vertexExample_"
    + this.stack.length + ',@options_'
    + this.stack.length + ')';
  this.bindVars["vertexExample_" + this.stack.length] = ex;
  this.bindVars["options_" + this.stack.length] = this.options;
  var stmt = new AQLStatement(query, "vertex");
  this.stack.push(stmt);
  this.lastVar = vertexName;
  this._path.push(vertexName);
  this._pathVertices.push(vertexName);
  return this;

};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_vertices
/// `graph_query.vertices(examples)`
/// *Select all vertices connected to the edges selected before.*
///
/// Creates an AQL statement to select all vertices for each of the edges selected
/// in the step before.
/// This includes all vertices contained in *_from* as well as *_to* attribute of the edges.
/// The resulting set of vertices can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLVerticesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.vertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered vertices by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLVerticesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.vertices({name: "Alice"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered vertices by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLVerticesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.vertices([{name: "Alice"}, {name: "Charly"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.vertices = function(example) {
  this._addToPrint("vertices", example);
  if (!this.getLastVar()) {
    return this._vertices(example);
  }
  var edgeVar = this.getLastVar();
  this._vertices(example);
  var vertexVar = this.getLastVar();
  var query = "FILTER " + edgeVar
    + "._from == " + vertexVar
    + "._id || " + edgeVar
    + "._to == " + vertexVar
    + "._id";
  var stmt = new AQLStatement(query);
  this.stack.push(stmt);
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_fromVertices
/// `graph_query.fromVertices(examples)`
/// *Select all source vertices of the edges selected before.*
/// 
///
/// Creates an AQL statement to select the set of vertices where the edges selected
/// in the step before start at.
/// This includes all vertices contained in *_from* attribute of the edges.
/// The resulting set of vertices can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered source vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLFromVerticesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.fromVertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered source vertices by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLFromVerticesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.fromVertices({name: "Alice"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered source vertices by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLFromVerticesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.fromVertices([{name: "Alice"}, {name: "Charly"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.fromVertices = function(example) {
  this._addToPrint("fromVertices", example);
  if (!this.getLastVar()) {
    return this._vertices(example);
  }
  var edgeVar = this.getLastVar();
  this._vertices(example);
  var vertexVar = this.getLastVar();
  var query = "FILTER " + edgeVar
    + "._from == " + vertexVar
    + "._id";
  var stmt = new AQLStatement(query);
  this.stack.push(stmt);
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_toVertices
/// `graph_query.toVertices(examples)`
/// *Select all vertices targeted by the edges selected before.*
/// 
///
/// Creates an AQL statement to select the set of vertices where the edges selected
/// in the step before end in.
/// This includes all vertices contained in *_to* attribute of the edges.
/// The resulting set of vertices can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// To request unfiltered target vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLToVerticesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered target vertices by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLToVerticesFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices({name: "Bob"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered target vertices by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLToVerticesFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices([{name: "Bob"}, {name: "Diana"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.toVertices = function(example) {
  this._addToPrint("toVertices", example);
  if (!this.getLastVar()) {
    return this._vertices(example);
  }
  var edgeVar = this.getLastVar();
  this._vertices(example);
  var vertexVar = this.getLastVar();
  var query = "FILTER " + edgeVar
    + "._to == " + vertexVar
    + "._id";
  var stmt = new AQLStatement(query);
  this.stack.push(stmt);
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Get the variable holding the last result
///
/// Only for internal use.
/// The return statement of the AQL query has to return
/// this value.
/// Also chaining has to use this variable to restrict
/// queries in the next step to only values from this set.
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.getLastVar = function() {
  if (this.lastVar === "") {
    return false;
  }
  return this.lastVar;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_path
/// `graph_query.path()`
/// *The result of the query is the path to all elements.*
///
/// By defaut the result of the generated AQL query is the set of elements passing the last matches.
/// So having a `vertices()` query as the last step the result will be set of vertices.
/// Using `path()` as the last action before requesting the result
/// will modify the result such that the path required to find the set vertices is returned.
///
/// *Examples*
///
/// Request the iteratively explored path using vertices and edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLPathSimple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.outEdges().toVertices().path().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// When requesting neighbors the path to these neighbors is expanded:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLPathNeighbors}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.neighbors().path().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.path = function() {
  this._clearCursor();
  var statement = new AQLStatement("", "path");
  this.stack.push(statement);
  return this;
};

AQLGenerator.prototype.pathVertices = function() {
  this._clearCursor();
  var statement = new AQLStatement("", "pathVertices");
  this.stack.push(statement);
  return this;
};

AQLGenerator.prototype.pathEdges = function() {
  this._clearCursor();
  var statement = new AQLStatement("", "pathEdges");
  this.stack.push(statement);
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_neighbors
/// `graph_query.neighbors(examples, options)`
/// *Select all neighbors of the vertices selected in the step before.*
/// 
///
/// Creates an AQL statement to select all neighbors for each of the vertices selected
/// in the step before.
/// The resulting set of vertices can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *edgeExamples*: Filter the edges to be followed, see [Definition of examples](#definition_of_examples)
///   * *edgeCollectionRestriction* : One or a list of edge-collection names that should be
///       considered to be on the path.
///   * *vertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered on the intermediate vertex steps.
///   * *minDepth*: Defines the minimal number of intermediate steps to neighbors (default is 1).
///   * *maxDepth*: Defines the maximal number of intermediate steps to neighbors (default is 1).
///
/// *Examples*
///
/// To request unfiltered neighbors:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLNeighborsUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.neighbors().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered neighbors by a single example:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLNeighborsFilteredSingle}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.neighbors({name: "Bob"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered neighbors by multiple examples:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLNeighborsFilteredMultiple}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.vertices([{name: "Bob"}, {name: "Charly"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.neighbors = function(vertexExample, options) {
  this._addToPrint("neighbors", vertexExample, options);
  var ex = transformExample(vertexExample);
  var resultName = "neighbors_" + this.stack.length;
  var query = "FOR " + resultName
    + " IN GRAPH_NEIGHBORS(@graphName,"
    + this.getLastVar()
    + ',@options_'
    + this.stack.length + ')';
  var opts;
  if (options) {
    opts = _.clone(options);
  } else {
    opts = {};
  }
  opts.neighborExamples = ex;
  this.bindVars["options_" + this.stack.length] = opts;
  var stmt = new AQLStatement(query, "neighbor");
  this.stack.push(stmt);
  this.lastVar = resultName + ".vertex";
  this._path.push(resultName + ".path");
  this._pathVertices.push("SLICE(" + resultName + ".path.vertices, 1)");
  this._pathEdges.push(resultName + ".path.edges");
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Get the last statement that can be restricted to collections
///
/// Only for internal use.
/// This returnes the last statement that can be restricted to
/// specific collections.
/// Required to allow a chaining of `restrict` after `filter` for instance.
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype._getLastRestrictableStatementInfo = function() {
  var i = this.stack.length - 1;
  while (!this.stack[i].allowsRestrict()) {
    i--;
  }
  return {
    statement: this.stack[i],
    options: this.bindVars["options_" + i]
  };
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_restrict
/// `graph_query.restrict(restrictions)`
/// *Restricts the last statement in the chain to return
/// only elements of a specified set of collections*
///
///
/// By default all collections in the graph are searched for matching elements
/// whenever vertices and edges are requested.
/// Using *restrict* after such a statement allows to restrict the search
/// to a specific set of collections within the graph.
/// Restriction is only applied to this one part of the query.
/// It does not effect earlier or later statements.
/// 
/// *Parameter*
///
/// * *restrictions*: Define either one or a list of collections in the graph.
///     Only elements from these collections are taken into account for the result.
///
/// *Examples*
///
/// Request all directly connected vertices unrestricted:
/// 
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLUnrestricted}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.edges().vertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Apply a restriction to the directly connected vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLRestricted}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.edges().vertices().restrict("female").toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Restriction of a query is only valid for collections known to the graph:
//
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLRestrictedUnknown}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices({name: "Alice"});
///   query.edges().vertices().restrict(["female", "male", "products"]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.restrict = function(restrictions) {
  this._addToPrint("restrict", restrictions);
  this._clearCursor();
  var rest = stringToArray(restrictions);
  var lastQueryInfo = this._getLastRestrictableStatementInfo();
  var lastQuery = lastQueryInfo.statement;
  var opts = lastQueryInfo.options;
  var restricts;
  if (lastQuery.isEdgeQuery()) {
    checkAllowsRestriction(
      this.graph.__edgeCollections,
      rest,
      "edge collections"
    );
    restricts = opts.edgeCollectionRestriction || [];
    opts.edgeCollectionRestriction = restricts.concat(restrictions);
  } else if (lastQuery.isVertexQuery() || lastQuery.isNeighborQuery()) {
    checkAllowsRestriction(
      this.graph.__vertexCollections,
      rest,
      "vertex collections"
    );
    restricts = opts.vertexCollectionRestriction || [];
    opts.vertexCollectionRestriction = restricts.concat(restrictions);
  }
  return this;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_filter
/// `graph_query.filter(examples)`
/// *Filter the result of the query*
///
/// This can be used to further specfiy the expected result of the query.
/// The result set is reduced to the set of elements that matches the given *examples*.
/// 
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// Request vertices unfiltered:
///  
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLUnfilteredVertices}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Request vertices filtered:
///  
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLFilteredVertices}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices().filter({name: "Alice"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Request edges unfiltered:
///  
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLUnfilteredEdges}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices().outEdges().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Request edges filtered:
///  
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLFilteredEdges}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._edges({type: "married"});
///   query.toVertices().outEdges().filter({type: "married"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.filter = function(example) {
  this._addToPrint("filter", example);
  this._clearCursor();
  var ex = [];
  if (Object.prototype.toString.call(example) !== "[object Array]") {
    if (Object.prototype.toString.call(example) !== "[object Object]") {
      var err = new ArangoError();
      err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_EXAMPLE_ARRAY_OBJECT.code;
      err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_EXAMPLE_ARRAY_OBJECT.message;
      throw err;
    }
    ex = [example];
  } else {
    ex = example;
  }
  var query = "FILTER MATCHES(" + this.getLastVar() + "," + JSON.stringify(ex) + ")";
  this.stack.push(new AQLStatement(query));
  return this;
};

AQLGenerator.prototype.printQuery = function() {
  return this.stack.map(function(stmt) {
    return stmt.printQuery();
  }).join(" ");
};

AQLGenerator.prototype.execute = function() {
  this._clearCursor();
  var query = this.printQuery();
  var bindVars = this.bindVars;
  if (this.stack[this.stack.length-1].isPathQuery()) {
    query += " RETURN [" + this._path + "]";
  } else if (this.stack[this.stack.length-1].isPathVerticesQuery()) {
    query += " RETURN FLATTEN([" + this._pathVertices + "])";
  } else if (this.stack[this.stack.length-1].isPathEdgesQuery()) {
    query += " RETURN FLATTEN([" + this._pathEdges + "])";
  } else {
    query += " RETURN " + this.getLastVar();
  }
  return db._query(query, bindVars, {count: true});
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_toArray
/// `graph_query.toArray()`
/// *Returns an array containing the complete result.*
///
///
/// This function executes the generated query and returns the
/// entire result as one array.
/// ToArray does not return the generated query anymore and
/// hence can only be the endpoint of a query.
/// However keeping a reference to the query before
/// executing allows to chain further statements to it.
///
/// *Examples*
///
/// To collect the entire result of a query toArray can be used:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLToArray}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
///   query.toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.toArray = function() {
  this._createCursor();
  return this.cursor.toArray();
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_count
/// `graph_query.count()`
/// *Returns the number of returned elements if the query is executed.*
///
/// This function determines the amount of elements to be expected within the result of the query.
/// It can be used at the beginning of execution of the query
/// before using *next()* or in between *next()* calls.
/// The query object maintains a cursor of the query for you.
/// *count()* does not change the cursor position.
///
/// *Examples*
///
/// To count the number of matched elements: 
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLCount}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
///   query.count();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.count = function() {
  this._createCursor();
  return this.cursor.count();
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_hasNext
/// `graph_query.hasNext()`
/// *Checks if the query has further results.*
///
/// The generated statement maintains a cursor for you.
/// If this cursor is already present *hasNext()* will
/// use this cursors position to determine if there are
/// further results available.
/// If the query has not yet been executed *hasNext()*
/// will execute it and create the cursor for you.
///
/// *Examples*
///
/// Start query execution with hasNext: 
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLHasNext}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
///   query.hasNext();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Iterate over the result as long as it has more elements:
/// 
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLHasNextIteration}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
/// | while (query.hasNext()) {
/// |   var entry = query.next();
/// |   // Do something with the entry
///   }
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
/// 
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.hasNext = function() {
  this._createCursor();
  return this.cursor.hasNext();
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fluent_aql_next
/// `graph_query.next()`
/// *Request the next element in the result.*
///
/// The generated statement maintains a cursor for you.
/// If this cursor is already present *next()* will
/// use this cursors position to deliver the next result.
/// Also the cursor position will be moved by one.
/// If the query has not yet been executed *next()*
/// will execute it and create the cursor for you.
/// It will throw an error of your query has no further results.
///
/// *Examples*
///
/// Request some elements with next: 
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLNext}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
///   query.next();
///   query.next();
///   query.next();
///   query.next();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// The cursor is recreated if the query is changed:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphFluentAQLNextRecreate}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   var query = graph._vertices();
///   query.next();
///   query.edges();
///   query.next();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

AQLGenerator.prototype.next = function() {
  this._createCursor();
  return this.cursor.next();
};

// -----------------------------------------------------------------------------
// --SECTION--                                                 public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_undirectedRelation
/// `graph_module._undirectedRelation(relationName, vertexCollections)`
/// *Define an undirected relation.*
///
/// Defines an undirected relation with the name *relationName* using the
/// list of *vertexCollections*. This relation allows the user to store
/// edges in any direction between any pair of vertices within the
/// *vertexCollections*.
///
/// *Parameter*
///
/// * *relationName*: The name of the edge collection where the edges should be stored.
///     Will be created if it does not yet exist.
/// * *vertexCollections*: One or a list of collection names for which connections are allowed.
///     Will be created if they do not exist.
///
/// *Examples*
///
/// To define simple relation with only one vertex collection:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphUndirectedRelationDefinition1}
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._undirectedRelation("friend", "user");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To define a relation between several vertex collections:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphUndirectedRelationDefinition2}
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._undirectedRelation("marriage", ["female", "male"]);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////


var _undirectedRelation = function (relationName, vertexCollections) {
  var err;
  if (arguments.length < 2) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_NUMBER_OF_ARGUMENTS.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_NUMBER_OF_ARGUMENTS.message + "2";
    throw err;
  }

  if (typeof relationName !== "string" || relationName === "") {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.message + " arg1 must not be empty";
    throw err;
  }

  if (!isValidCollectionsParameter(vertexCollections)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.message + " arg2 must not be empty";
    throw err;
  }

  return {
    collection: relationName,
    from: stringToArray(vertexCollections),
    to: stringToArray(vertexCollections)
  };
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_directedRelation
/// `graph_module._directedRelation(relationName, fromVertexCollections, toVertexCollections)`
/// *Define a directed relation.*
///
/// The *relationName* defines the name of this relation and references to the underlying edge collection.
/// The *fromVertexCollections* is an Array of document collections holding the start vertices.
/// The *toVertexCollections* is an Array of document collections holding the target vertices.
/// Relations are only allowed in the direction from any collection in *fromVertexCollections*
/// to any collection in *toVertexCollections*.
///
/// *Parameter*
///
/// * *relationName*: The name of the edge collection where the edges should be stored.
///     Will be created if it does not yet exist.
/// * *fromVertexCollections*: One or a list of collection names. Source vertices for the edges
///     have to be stored in these collections. Collections will be created if they do not exist.
/// * *toVertexCollections*: One or a list of collection names. Target vertices for the edges
///     have to be stored in these collections. Collections will be created if they do not exist.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphDirectedRelationDefinition}
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._directedRelation("has_bought", ["Customer", "Company"], ["Groceries", "Electronics"]);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////


var _directedRelation = function (
  relationName, fromVertexCollections, toVertexCollections) {
  var err;
  if (arguments.length < 3) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_NUMBER_OF_ARGUMENTS.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_NUMBER_OF_ARGUMENTS.message + "3";
    throw err;
  }

  if (typeof relationName !== "string" || relationName === "") {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.message + " arg1 must be non empty string";
    throw err;
  }

  if (!isValidCollectionsParameter(fromVertexCollections)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.message
      + " arg2 must be non empty string or array";
    throw err;
  }

  if (!isValidCollectionsParameter(toVertexCollections)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_INVALID_PARAMETER.message
      + " arg3 must be non empty string or array";
    throw err;
  }

  return {
    collection: relationName,
    from: stringToArray(fromVertexCollections),
    to: stringToArray(toVertexCollections)
  };
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_list
/// `graph_module._list()`
/// *List all graphs.*
///
/// Lists all graph names stored in this database.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphList}
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._list();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

var _list = function() {
  var gdb = getGraphCollection();
  return _.pluck(gdb.toArray(), "_key");
};


var _listObjects = function() {
  return getGraphCollection().toArray();
};




////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edge_definitions
/// `graph_module._edgeDefinitions(relation1, relation2, ..., relationN)`
/// *Create a list of edge definitions to construct a graph.*
///
/// The list of edge definitions of a graph can be managed by the graph module itself.
/// This function is the entry point for the management and will return the correct list.
///
/// *Parameter*
///
/// * *relationX*: An object representing a definition of one relation in the graph
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeDefinitions}
///   var graph_module = require("org/arangodb/general-graph");
///   directed_relation = graph_module._directedRelation("lives_in", "user", "city");
///   undirected_relation = graph_module._undirectedRelation("knows", "user");
///   edgedefinitions = graph_module._edgeDefinitions(directed_relation, undirected_relation);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////


var _edgeDefinitions = function () {

  var res = [], args = arguments;
  Object.keys(args).forEach(function (x) {
   res.push(args[x]);
  });

  return res;

};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_extend_edge_definitions
/// `graph_module._extendEdgeDefinitions(edgeDefinitions, relation1, relation2, ..., relationN)`
/// *Extend the list of edge definitions to construct a graph.*
///
/// In order to add more edge definitions to the graph before creating
/// this function can be used to add more definitions to the initial list.
///
/// *Parameter*
///
/// * *edgeDefinitions*: A list of relation definition objects.
/// * *relationX*: An object representing a definition of one relation in the graph
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeDefinitionsExtend}
///   var graph_module = require("org/arangodb/general-graph");
///   directed_relation = graph_module._directedRelation("lives_in", "user", "city");
///   undirected_relation = graph_module._directedRelation("knows", "user");
///   edgedefinitions = graph_module._edgeDefinitions(directed_relation);
///   edgedefinitions = graph_module._extendEdgeDefinitions(undirected_relation);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

var _extendEdgeDefinitions = function (edgeDefinition) {
  var args = arguments, i = 0;

  Object.keys(args).forEach(
    function (x) {
      i++;
      if (i === 1) {
        return;
      }
      edgeDefinition.push(args[x]);
    }
  );
};
////////////////////////////////////////////////////////////////////////////////
/// internal helper to sort a graph's edge definitions
////////////////////////////////////////////////////////////////////////////////
var sortEdgeDefinition = function(edgeDefinition) {
  edgeDefinition.from = edgeDefinition.from.sort();
  edgeDefinition.to = edgeDefinition.to.sort();
  return edgeDefinition;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new graph
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_how_to_create
///
/// * Create a graph
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphCreateGraphHowTo1}
///   var graph_module = require("org/arangodb/general-graph");
///   var graph = graph_module._create("myGraph");
///   graph;
/// ~ graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// * Add some vertex collections
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphCreateGraphHowTo2}
/// ~ var graph_module = require("org/arangodb/general-graph");
/// ~ var graph = graph_module._create("myGraph");
///   graph._addVertexCollection("shop");
///   graph._addVertexCollection("customer");
///   graph._addVertexCollection("pet");
///   graph;
/// ~ graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// * Define relations on the
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphCreateGraphHowTo3}
/// ~ var graph_module = require("org/arangodb/general-graph");
/// ~ var graph = graph_module._create("myGraph");
///   var rel = graph_module._directedRelation("isCustomer", ["shop"], ["customer"]);
///   graph._extendEdgeDefinitions(rel);
///   graph;
/// ~ graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_create
/// `graph_module._create(graphName, edgeDefinitions, orphanCollections)`
/// *Create a graph*
///
///
/// The creation of a graph requires the name of the graph and a definition of its edges.
///
/// For every type of edge definition a convenience method exists that can be used to create a graph.
/// Optionally a list of vertex collections can be added, which are not used in any edge definition.
/// These collections are referred to as orphan collections within this chapter.
/// All collections used within the creation process are created if they do not exist.
///
/// *Parameter*
///
/// * *graphName*: Unique identifier of the graph
/// * *edgeDefinitions* (optional): List of relation definition objects
/// * *orphanCollections* (optional): List of additional vertex collection names
///
/// *Examples*
///
/// Create an empty graph, edge definitions can be added at runtime:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphCreateGraph}
///   var graph_module = require("org/arangodb/general-graph");
///   graph = graph_module._create("myGraph");
/// ~ graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Create a graph with edge definitions and orphan collections:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphCreateGraph2}
///   var graph_module = require("org/arangodb/general-graph");
/// | graph = graph_module._create("myGraph",
///   [graph_module._undirectedRelation("myRelation", ["male", "female"])], ["sessions"]);
/// ~ graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

var _create = function (graphName, edgeDefinitions, orphanCollections) {

  if (! Array.isArray(orphanCollections) ) {
    orphanCollections = [];
  }
  var gdb = getGraphCollection(),
    err,
    graphAlreadyExists = true,
    collections,
    result;
  if (!graphName) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_CREATE_MISSING_NAME.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_CREATE_MISSING_NAME.message;
    throw err;
  }
  edgeDefinitions = edgeDefinitions || [];
  if (!Array.isArray(edgeDefinitions)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_CREATE_MALFORMED_EDGE_DEFINITION.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_CREATE_MALFORMED_EDGE_DEFINITION.message;
    throw err;
  }
  //check, if a collection is already used in a different edgeDefinition
  var tmpCollections = [];
  var tmpEdgeDefinitions = {};
  edgeDefinitions.forEach(
    function(edgeDefinition) {
      var col = edgeDefinition.collection;
      if (tmpCollections.indexOf(col) !== -1) {
        err = new ArangoError();
        err.errorNum = arangodb.errors.ERROR_GRAPH_COLLECTION_MULTI_USE.code;
        err.errorMessage = arangodb.errors.ERROR_GRAPH_COLLECTION_MULTI_USE.message;
        throw err;
      }
      tmpCollections.push(col);
      tmpEdgeDefinitions[col] = edgeDefinition;
    }
  );
  gdb.toArray().forEach(
    function(singleGraph) {
      var sGEDs = singleGraph.edgeDefinitions;
      sGEDs.forEach(
        function(sGED) {
          var col = sGED.collection;
          if (tmpCollections.indexOf(col) !== -1) {
            if (JSON.stringify(sGED) !== JSON.stringify(tmpEdgeDefinitions[col])) {
              err = new ArangoError();
              err.errorNum = arangodb.errors.ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS.code;
              err.errorMessage = col + " "
                + arangodb.errors.ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS.message;
              throw err;
            }
          }
        }
      );
    }
  );

  try {
    var g = gdb.document(graphName);
  } catch (e) {
    if (e.errorNum !== errors.ERROR_ARANGO_DOCUMENT_NOT_FOUND.code) {
      throw e;
    }
    graphAlreadyExists = false;
  }

  if (graphAlreadyExists) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_DUPLICATE.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_DUPLICATE.message;
    throw err;
  }

  collections = findOrCreateCollectionsByEdgeDefinitions(edgeDefinitions, false);
  orphanCollections.forEach(
    function(oC) {
      findOrCreateCollectionByName(oC, ArangoCollection.TYPE_DOCUMENT);
    }
  );

  edgeDefinitions.forEach(
    function(eD, index) {
      var tmp = sortEdgeDefinition(eD);
      edgeDefinitions[index] = tmp;
    }
  );
  orphanCollections = orphanCollections.sort();

  var data =  gdb.save({
    'orphanCollections' : orphanCollections,
    'edgeDefinitions' : edgeDefinitions,
    '_key' : graphName
  });

  result = new Graph(graphName, edgeDefinitions, collections[0], collections[1],
    orphanCollections, data._rev , data._id);
  return result;

};

var createHiddenProperty = function(obj, name, value) {
  Object.defineProperty(obj, name, {
    enumerable: false,
    writable: true
  });
  obj[name] = value;
};
////////////////////////////////////////////////////////////////////////////////
/// @brief helper for updating binded collections
////////////////////////////////////////////////////////////////////////////////
var removeEdge = function (edgeId, options, self) {
  options = options || {};
  var edgeCollection = edgeId.split("/")[0];
  var graphs = getGraphCollection().toArray();
  self.__idsToRemove.push(edgeId);
  self.__collectionsToLock.push(edgeCollection);
  graphs.forEach(
    function(graph) {
      var edgeDefinitions = graph.edgeDefinitions;
      if (graph.edgeDefinitions) {
        edgeDefinitions.forEach(
          function(edgeDefinition) {
            var from = edgeDefinition.from;
            var to = edgeDefinition.to;
            var collection = edgeDefinition.collection;
            // if collection of edge to be deleted is in from or to
            if (from.indexOf(edgeCollection) !== -1 || to.indexOf(edgeCollection) !== -1) {
              //search all edges of the graph
              var edges = db[collection].toArray();
              edges.forEach(
                function (edge) {
                  // if from is
                  if(self.__idsToRemove.indexOf(edge._id) === -1) {
                    if (edge._from === edgeId || edge._to === edgeId) {
                      removeEdge(edge._id, options, self);
                    }
                  }
                }
              );
            }
          }
        );
      }
    }
  );
  return;
};

var bindEdgeCollections = function(self, edgeCollections) {
  _.each(edgeCollections, function(key) {
    var obj = db._collection(key);
    var wrap = wrapCollection(obj);
    // save
    var old_save = wrap.save;
    wrap.save = function(from, to, data) {
      //check, if edge is allowed
      self.__edgeDefinitions.forEach(
        function(edgeDefinition) {
          if (edgeDefinition.collection === key) {
            var fromCollection = from.split("/")[0];
            var toCollection = to.split("/")[0];
            if (! _.contains(edgeDefinition.from, fromCollection)
              || ! _.contains(edgeDefinition.to, toCollection)) {
              var err = new ArangoError();
              err.errorNum = arangodb.errors.ERROR_GRAPH_INVALID_EDGE.code;
              err.errorMessage =
                arangodb.errors.ERROR_GRAPH_INVALID_EDGE.message + " between " + from + " and " + to + ".";
              throw err;
            }
          }
        }
      );
      return old_save(from, to, data);
    };

    // remove
    wrap.remove = function(edgeId, options) {
      var result;
      //if _key make _id (only on 1st call)
      if (edgeId.indexOf("/") === -1) {
        edgeId = key + "/" + edgeId;
      }
      removeEdge(edgeId, options, self);

      try {
        db._executeTransaction({
          collections: {
            write: self.__collectionsToLock
          },
          action: function (params) {
            var db = require("internal").db;
            params.ids.forEach(
              function(edgeId) {
                if (params.options) {
                  db._remove(edgeId, params.options);
                } else {
                  db._remove(edgeId);
                }
              }
            );
          },
          params: {
            ids: self.__idsToRemove,
            options: options
          }
        });
        result = true;
      } catch (e) {
        result = false;
      }
      self.__idsToRemove = [];
      self.__collectionsToLock = [];
      return result;
    };

    self[key] = wrap;
  });
};

var bindVertexCollections = function(self, vertexCollections) {
  _.each(vertexCollections, function(key) {
    var obj = db._collection(key);
    var result;
    var wrap = wrapCollection(obj);
    wrap.remove = function(vertexId, options) {
      //delete all edges using the vertex in all graphs
      var graphs = getGraphCollection().toArray();
      var vertexCollectionName = vertexId.split("/")[0];
      self.__collectionsToLock.push(vertexCollectionName);
      graphs.forEach(
        function(graph) {
          var edgeDefinitions = graph.edgeDefinitions;
          if (graph.edgeDefinitions) {
            edgeDefinitions.forEach(
              function(edgeDefinition) {
                var from = edgeDefinition.from;
                var to = edgeDefinition.to;
                var collection = edgeDefinition.collection;
                if (from.indexOf(vertexCollectionName) !== -1
                  || to.indexOf(vertexCollectionName) !== -1
                  ) {
                  var edges = db._collection(collection).toArray();
                  edges.forEach(
                    function(edge) {
                      if (edge._from === vertexId || edge._to === vertexId) {
                        removeEdge(edge._id, options, self);
                      }
                    }
                  );
                }
              }
            );
          }
        }
      );

      try {
        db._executeTransaction({
          collections: {
            write: self.__collectionsToLock
          },
          action: function (params) {
            var db = require("internal").db;
            params.ids.forEach(
              function(edgeId) {
                if (params.options) {
                  db._remove(edgeId, params.options);
                } else {
                  db._remove(edgeId);
                }
              }
            );
            if (params.options) {
              db._remove(params.vertexId, params.options);
            } else {
              db._remove(params.vertexId);
            }
          },
          params: {
            ids: self.__idsToRemove,
            options: options,
            vertexId: vertexId
          }
        });
        result = true;
      } catch (e) {
        result = false;
      }
      self.__idsToRemove = [];
      self.__collectionsToLock = [];

      return result;
    };
    self[key] = wrap;
  });

};
var updateBindCollections = function(graph) {
  //remove all binded collections
  Object.keys(graph).forEach(
    function(key) {
      if(key.substring(0,1) !== "_") {
        delete graph[key];
      }
    }
  );
  graph.__edgeDefinitions.forEach(
    function(edgeDef) {
      bindEdgeCollections(graph, [edgeDef.collection]);
      bindVertexCollections(graph, edgeDef.from);
      bindVertexCollections(graph, edgeDef.to);
    }
  );
  bindVertexCollections(graph, graph.__orphanCollections);
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_vertex_collection_save
/// `graph.vertexCollectionName.save(data)`
/// *Create a new vertex in vertexCollectionName*
///
/// *Parameter*
///
/// * *data*: JSON data of vertex.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVertexCollectionSave}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.male.save({name: "Floyd", _key: "floyd"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_vertex_collection_replace
/// `graph.vertexCollectionName.replace(vertexId, data, options)`
/// *Replaces the data of a vertex in collection vertexCollectionName*
///
/// *Parameter*
///
/// * *vertexId*: *_id* attribute of the vertex
/// * *data*: JSON data of vertex.
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVertexCollectionReplace}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.male.save({neym: "Jon", _key: "john"});
///   graph.male.replace("male/john", {name: "John"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_vertex_collection_update
/// `graph.vertexCollectionName.update(vertexId, data, options)`
/// *Updates the data of a vertex in collection vertexCollectionName*
///
/// *Parameter*
///
/// * *vertexId*: *_id* attribute of the vertex
/// * *data*: JSON data of vertex.
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVertexCollectionUpdate}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.female.save({name: "Lynda", _key: "linda"});
///   graph.female.update("female/linda", {name: "Linda", _key: "linda"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_vertex_collection_remove
/// `graph.vertexCollectionName.remove(vertexId, options)`
/// Removes a vertex in collection *vertexCollectionName*
///
///
/// Additionally removes all ingoing and outgoing edges of the vertex recursively
/// (see [edge remove](#edge.remove)).
///
/// *Parameter*
///
/// * *vertexId*: *_id* attribute of the vertex
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVertexCollectionRemove}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.male.save({name: "Kermit", _key: "kermit"});
///   db._exists("male/kermit")
///   graph.male.remove("male/kermit")
///   db._exists("male/kermit")
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edge_collection_save
/// `graph.edgeCollectionName.save(from, to, data, options)`
/// *Creates an edge from vertex *from* to vertex *to* in collection edgeCollectionName*
///
/// *Parameter*
///
/// * *from*: *_id* attribute of the source vertex
/// * *to*: *_id* attribute of the target vertex
/// * *data*: JSON data of the edge
/// * *options* (optional): See [collection documentation](../Edges/EdgeMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeCollectionSave1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.relation.save("male/bob", "female/alice", {type: "married", _key: "bobAndAlice"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// If the collections of *from* and *to* are not defined in an edge definition of the graph,
/// the edge will not be stored.
///
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeCollectionSave2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.relation.save("relation/aliceAndBob", "female/alice", {type: "married", _key: "bobAndAlice"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edge_collection_replace
/// `graph.edgeCollectionName.replace(edgeId, data, options)`
/// *Replaces the data of an edge in collection edgeCollectionName*
///
/// *Parameter*
///
/// * *edgeId*: *_id* attribute of the edge
/// * *data*: JSON data of the edge
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeCollectionReplace}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.relation.save("female/alice", "female/diana", {typo: "nose", _key: "aliceAndDiana"});
///   graph.relation.replace("relation/aliceAndDiana", {type: "knows"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edge_collection_update
/// `graph.edgeCollectionName.update(edgeId, data, options)`
/// *Updates the data of an edge in collection edgeCollectionName*
///
/// *Parameter*
///
/// * *edgeId*: *_id* attribute of the edge
/// * *data*: JSON data of the edge
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeCollectionUpdate}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.relation.save("female/alice", "female/diana", {type: "knows", _key: "aliceAndDiana"});
///   graph.relation.update("relation/aliceAndDiana", {type: "quarrelled", _key: "aliceAndDiana"});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edge_collection_remove
/// `graph.edgeCollectionName.remove(edgeId, options)`
/// *Removes an edge in collection edgeCollectionName*
///
/// *Parameter*
///
/// * *edgeId*: *_id* attribute of the edge
/// * *options* (optional): See [collection documentation](../Documents/DocumentMethods.md)
///
/// If this edge is used as a vertex by another edge, the other edge will be removed (recursively).
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgeCollectionRemove}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph.relation.save("female/alice", "female/diana", {_key: "aliceAndDiana"});
///   db._exists("relation/aliceAndDiana")
///   graph.relation.remove("relation/aliceAndDiana")
///   db._exists("relation/aliceAndDiana")
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
var Graph = function(graphName, edgeDefinitions, vertexCollections, edgeCollections,
                     orphanCollections, revision, id) {
  edgeDefinitions.forEach(
    function(eD, index) {
      var tmp = sortEdgeDefinition(eD);
      edgeDefinitions[index] = tmp;
    }
  );

  if (!orphanCollections) {
    orphanCollections = [];
  }
  var self = this;
  // Create Hidden Properties
  createHiddenProperty(this, "__name", graphName);
  createHiddenProperty(this, "__vertexCollections", vertexCollections);
  createHiddenProperty(this, "__edgeCollections", edgeCollections);
  createHiddenProperty(this, "__edgeDefinitions", edgeDefinitions);
  createHiddenProperty(this, "__idsToRemove", []);
  createHiddenProperty(this, "__collectionsToLock", []);
  createHiddenProperty(this, "__id", id);
  createHiddenProperty(this, "__rev", revision);
  createHiddenProperty(this, "__orphanCollections", orphanCollections);
  updateBindCollections(self);

};



////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_graph
/// `graph_module._graph(graphName)`
/// *Get a graph*
///
/// A graph can be get by its name.
///
/// *Parameter*
///
/// * *graphName*: Unique identifier of the graph
///
/// *Examples*
///
/// Get a graph:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphLoadGraph}
/// ~ var examples = require("org/arangodb/graph-examples/example-graph.js");
/// ~ var g1 = examples.loadGraph("social");
///   var graph_module = require("org/arangodb/general-graph");
///   graph = graph_module._graph("social");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

var _graph = function(graphName) {

  var gdb = getGraphCollection(),
    g, collections, orphanCollections;

  try {
    g = gdb.document(graphName);
  } 
  catch (e) {
    if (e.errorNum !== errors.ERROR_ARANGO_DOCUMENT_NOT_FOUND.code) {
      throw e;
    }
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_FOUND.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_FOUND.message;
    throw err;
  }

  collections = findOrCreateCollectionsByEdgeDefinitions(g.edgeDefinitions, true);
  orphanCollections = g.orphanCollections;
  if (!orphanCollections) {
    orphanCollections = [];
  }

  return new Graph(graphName, g.edgeDefinitions, collections[0], collections[1], orphanCollections,
    g._rev , g._id);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a graph exists.
////////////////////////////////////////////////////////////////////////////////

var _exists = function(graphId) {
  var gCol = getGraphCollection();
  return gCol.exists(graphId);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Helper for dropping collections of a graph.
////////////////////////////////////////////////////////////////////////////////

var checkIfMayBeDropped = function(colName, graphName, graphs) {
  var result = true;
  graphs.forEach(
    function(graph) {
      if (graph._key === graphName) {
        return;
      }
      var edgeDefinitions = graph.edgeDefinitions;
      if (edgeDefinitions) {
        edgeDefinitions.forEach(
          function(edgeDefinition) {
            var from = edgeDefinition.from;
            var to = edgeDefinition.to;
            var collection = edgeDefinition.collection;
            if (collection === colName
              || from.indexOf(colName) !== -1
              || to.indexOf(colName) !== -1
              ) {
              result = false;
            }
          }
        );
      }

      var orphanCollections = graph.orphanCollections;
      if (orphanCollections) {
        if (orphanCollections.indexOf(colName) !== -1) {
          result = false;
        }
      }
    }
  );

  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_drop
/// `graph_module._drop(graphName, dropCollections)`
/// *Remove a graph*
///
/// A graph can be dropped by its name.
/// This will automatically drop all collections contained in the graph as
/// long as they are not used within other graphs.
/// To drop the collections, the optional parameter *drop-collections* can be set to *true*.
///
/// *Parameter*
///
/// * *graphName*: Unique identifier of the graph
/// * *dropCollections* (optional): Define if collections should be dropped (default: false)
///
/// *Examples*
///
/// Drop a graph and keep collections:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphDropGraphKeep}
/// ~ var examples = require("org/arangodb/graph-examples/example-graph.js");
/// ~ var g1 = examples.loadGraph("social");
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._drop("social");
///   db._collection("female");
///   db._collection("male");
///   db._collection("relation");
/// ~ db._drop("female");
/// ~ db._drop("male");
/// ~ db._drop("relation");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphDropGraphDropCollections}
/// ~ var examples = require("org/arangodb/graph-examples/example-graph.js");
/// ~ var g1 = examples.loadGraph("social");
///   var graph_module = require("org/arangodb/general-graph");
///   graph_module._drop("social", true);
///   db._collection("female");
///   db._collection("male");
///   db._collection("relation");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

var _drop = function(graphId, dropCollections) {

  var gdb = getGraphCollection(),
    graphs;

  if (!gdb.exists(graphId)) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_FOUND.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_FOUND.message;
    throw err;
  }

  if (dropCollections === true) {
    var graph = gdb.document(graphId);
    var edgeDefinitions = graph.edgeDefinitions;
    edgeDefinitions.forEach(
      function(edgeDefinition) {
        var from = edgeDefinition.from;
        var to = edgeDefinition.to;
        var collection = edgeDefinition.collection;
        graphs = getGraphCollection().toArray();
        if (checkIfMayBeDropped(collection, graph._key, graphs)) {
          db._drop(collection);
        }
        from.forEach(
          function(col) {
            if (checkIfMayBeDropped(col, graph._key, graphs)) {
              db._drop(col);
            }
          }
        );
        to.forEach(
          function(col) {
            if (checkIfMayBeDropped(col, graph._key, graphs)) {
              db._drop(col);
            }
          }
        );
      }
    );
    //drop orphans
    graphs = getGraphCollection().toArray();
    if (!graph.orphanCollections) {
      graph.orphanCollections = [];
    }
    graph.orphanCollections.forEach(
      function(oC) {
        if (checkIfMayBeDropped(oC, graph._key, graphs)) {
          try {
            db._drop(oC);
          } catch (ignore) {}
        }
      }
    );
  }

  gdb.remove(graphId);
  return true;
};


////////////////////////////////////////////////////////////////////////////////
/// @brief return all edge collections of the graph.
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._edgeCollections = function() {
  return _.values(this.__edgeCollections);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief return all vertex collections of the graph.
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._vertexCollections = function() {
  var orphans = [];
  _.each(this.__orphanCollections, function(o) {
    orphans.push(db[o]);
  });
  return _.union(_.values(this.__vertexCollections), orphans);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief _EDGES(vertexId).
////////////////////////////////////////////////////////////////////////////////

// might be needed from AQL itself
Graph.prototype._EDGES = function(vertexId) {
  var err;
  if (vertexId.indexOf("/") === -1) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_FOUND.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_FOUND.message + ": " + vertexId;
    throw err;
  }
  var collection = vertexId.split("/")[0];
  if (!db._collection(collection)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message + ": " + collection;
    throw err;
  }

  var edgeCollections = this._edgeCollections();
  var result = [];

  edgeCollections.forEach(
    function(edgeCollection) {
      result = result.concat(edgeCollection.edges(vertexId));
    }
  );
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief INEDGES(vertexId).
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._INEDGES = function(vertexId) {
  var err;
  if (vertexId.indexOf("/") === -1) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_FOUND.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_FOUND.message + ": " + vertexId;
    throw err;
  }
  var collection = vertexId.split("/")[0];
  if (!db._collection(collection)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message + ": " + collection;
    throw err;
  }

  var edgeCollections = this._edgeCollections();
  var result = [];


  edgeCollections.forEach(
    function(edgeCollection) {
      result = result.concat(edgeCollection.inEdges(vertexId));
    }
  );
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief outEdges(vertexId).
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._OUTEDGES = function(vertexId) {
  var err;
  if (vertexId.indexOf("/") === -1) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_FOUND.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_FOUND.message + ": " + vertexId;
    throw err;
  }
  var collection = vertexId.split("/")[0];
  if (!db._collection(collection)) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message + ": " + collection;
    throw err;
  }

  var edgeCollections = this._edgeCollections();
  var result = [];


  edgeCollections.forEach(
    function(edgeCollection) {
      result = result.concat(edgeCollection.outEdges(vertexId));
    }
  );
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_edges
/// `graph._edges(examples)`
/// *Select some edges from the graph.*
///
/// Creates an AQL statement to select a subset of the edges stored in the graph.
/// This is one of the entry points for the fluent AQL interface.
/// It will return a mutable AQL statement which can be further refined, using the
/// functions described below.
/// The resulting set of edges can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// In the examples the *toArray* function is used to print the result.
/// The description of this module can be found below.
///
/// To request unfiltered edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._edges().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered edges:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphEdgesFiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._edges({type: "married"}).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._edges = function(edgeExample) {
  var AQLStmt = new AQLGenerator(this);
  // If no direction is specified all edges are duplicated.
  // => For initial requests a direction has to be set
  return AQLStmt.outEdges(edgeExample);
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_vertices
/// `graph._vertices(examples)`
/// *Select some vertices from the graph.*
///
/// Creates an AQL statement to select a subset of the vertices stored in the graph.
/// This is one of the entry points for the fluent AQL interface.
/// It will return a mutable AQL statement which can be further refined, using the
/// functions described below.
/// The resulting set of edges can be filtered by defining one or more *examples*.
///
/// *Parameter*
///
/// * *examples*: See [Definition of examples](#definition_of_examples)
///
/// *Examples*
///
/// In the examples the *toArray* function is used to print the result.
/// The description of this module can be found below.
///
/// To request unfiltered vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVerticesUnfiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._vertices().toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// To request filtered vertices:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphVerticesFiltered}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._vertices([{name: "Alice"}, {name: "Bob"}]).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._vertices = function(example) {
  var AQLStmt = new AQLGenerator(this);
  return AQLStmt.vertices(example);
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_fromVertex
/// `graph._fromVertex(edgeId)`
/// *Get the source vertex of an edge*
///
/// Returns the vertex defined with the attribute *_from* of the edge with *edgeId* as its *_id*.
///
/// *Parameter*
///
/// * *edgeId*: *_id* attribute of the edge
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphGetFromVertex}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._fromVertex("relation/aliceAndBob")
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._fromVertex = function(edgeId) {
  var edgeCollection = this._getEdgeCollectionByName(edgeId.split("/")[0]);
  var document = edgeCollection.document(edgeId);
  if (document) {
    var vertexId = document._from;
    var vertexCollection = this._getVertexCollectionByName(vertexId.split("/")[0]);
    return vertexCollection.document(vertexId);
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_toVertex
/// `graph._toVertex(edgeId)`
/// *Get the target vertex of an edge*
///
/// Returns the vertex defined with the attribute *_to* of the edge with *edgeId* as its *_id*.
///
/// *Parameter*
///
/// * *edgeId*: *_id* attribute of the edge
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphGetToVertex}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("social");
///   graph._toVertex("relation/aliceAndBob")
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._toVertex = function(edgeId) {
  var edgeCollection = this._getEdgeCollectionByName(edgeId.split("/")[0]);
  var document = edgeCollection.document(edgeId);
  if (document) {
    var vertexId = document._to;
    var vertexCollection = this._getVertexCollectionByName(vertexId.split("/")[0]);
    return vertexCollection.document(vertexId);
  }
};


////////////////////////////////////////////////////////////////////////////////
/// @brief get edge collection by name.
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._getEdgeCollectionByName = function(name) {
  if (this.__edgeCollections[name]) {
    return this.__edgeCollections[name];
  }
  var err = new ArangoError();
  err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
  err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message + ": " + name;
  throw err;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief get vertex collection by name.
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._getVertexCollectionByName = function(name) {
  if (this.__vertexCollections[name]) {
    return this.__vertexCollections[name];
  }
  var err = new ArangoError();
  err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
  err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message + ": " + name;
  throw err;
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_neighbors
/// `graph._neighbors(vertexExample, options)`
/// *Get all neighbors of the vertices defined by the example*
///
/// The function accepts an id, an example, a list of examples or even an empty
/// example as parameter for vertexExample.
/// The complexity of this method is **O(n\*m^x)** with *n* being the vertices defined by the
/// parameter vertexExamplex, *m* the average amount of neighbors and *x* the maximal depths.
/// Hence the default call would have a complexity of **O(n\*m)**;
///
/// *Parameter*
///
/// * *vertexExample*: See [Definition of examples](#definition_of_examples)
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *edgeExamples*: Filter the edges, see [Definition of examples](#definition_of_examples)
///   * *neighborExamples*: Filter the neighbor vertices, see [Definition of examples](#definition_of_examples)
///   * *edgeCollectionRestriction* : One or a list of edge-collection names that should be
///       considered to be on the path.
///   * *vertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered on the intermediate vertex steps.
///   * *minDepth*: Defines the minimal number of intermediate steps to neighbors (default is 1).
///   * *maxDepth*: Defines the maximal number of intermediate steps to neighbors (default is 1).
///
/// *Examples*
///
/// A route planner example, all neighbors of capitals.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleNeighbors1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._neighbors({isCapital : true});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, all outbound neighbors of Hamburg.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleNeighbors2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._neighbors('germanCity/Hamburg', {direction : 'outbound', maxDepth : 2});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._neighbors = function(vertexExample, options) {
  var AQLStmt = new AQLGenerator(this);
  // If no direction is specified all edges are duplicated.
  // => For initial requests a direction has to be set
  if (!options) {
    options = {};
  }
  return AQLStmt.vertices(vertexExample).neighbors(options.neighborExamples, options)
    .toArray();
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_common_neighbors
/// `graph._commonNeighbors(vertex1Example, vertex2Examples, optionsVertex1, optionsVertex2)`
/// *Get all common neighbors of the vertices defined by the examples.*
///
/// This function returns the intersection of *graph_module._neighbors(vertex1Example, optionsVertex1)*
/// and *graph_module._neighbors(vertex2Example, optionsVertex2)*.
/// For parameter documentation see [_neighbors](#_neighbors).
///
/// The complexity of this method is **O(n\*m^x)** with *n* being the maximal amount of vertices
/// defined by the parameters vertexExamples, *m* the average amount of neighbors and *x* the
/// maximal depths.
/// Hence the default call would have a complexity of **O(n\*m)**;
///
/// *Examples*
///
/// A route planner example, all common neighbors of capitals.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCommonNeighbors1}
/// ~ var db = require("internal").db;
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// graph._commonNeighbors({isCapital : true}, {isCapital : true});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, all common outbound neighbors of Hamburg with any other location
/// which have a maximal depth of 2 :
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCommonNeighbors2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
/// | graph._commonNeighbors(
/// |   'germanCity/Hamburg',
/// |   {},
/// |   {direction : 'outbound', maxDepth : 2},
///     {direction : 'outbound', maxDepth : 2});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._commonNeighbors = function(vertex1Example, vertex2Example, optionsVertex1, optionsVertex2) {

  var ex1 = transformExample(vertex1Example);
  var ex2 = transformExample(vertex2Example);
  var query = "FOR e"
    + " IN GRAPH_COMMON_NEIGHBORS(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options1'
    + ',@options2'
    + ')  SORT  ATTRIBUTES(e)[0] RETURN e';
  optionsVertex1 = optionsVertex1 || {};
  optionsVertex2 = optionsVertex2 || {};
  var bindVars = {
    "graphName": this.__name,
    "options1": optionsVertex1,
    "options2": optionsVertex2,
    "ex1": ex1,
    "ex2": ex2
  };
  return db._query(query, bindVars, {count: true}).toArray();
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_count_common_neighbors
/// `graph._countCommonNeighbors(vertex1Example, vertex2Examples, optionsVertex1, optionsVertex2)`
/// *Get the amount of common neighbors of the vertices defined by the examples.*
///
/// Similar to [_commonNeighbors](#_commonNeighbors) but returns count instead of the elements.
///
/// *Examples*
///
/// A route planner example, all common neighbors of capitals.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCommonNeighborsAmount1}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._countCommonNeighbors({isCapital : true}, {isCapital : true});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, all common outbound neighbors of Hamburg with any other location
/// which have a maximal depth of 2 :
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCommonNeighborsAmount2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
/// | graph._countCommonNeighbors('germanCity/Hamburg', {}, {direction : 'outbound', maxDepth : 2},
///   {direction : 'outbound', maxDepth : 2});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._countCommonNeighbors = function(vertex1Example, vertex2Example, optionsVertex1, optionsVertex2) {
  var ex1 = transformExample(vertex1Example);
  var ex2 = transformExample(vertex2Example);
  var query = "FOR e"
    + " IN GRAPH_COMMON_NEIGHBORS(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options1'
    + ',@options2'
    + ') FOR a in ATTRIBUTES(e) FOR b in ATTRIBUTES(e[a])  '
    + 'SORT  ATTRIBUTES(e)[0] RETURN [a, b, LENGTH(e[a][b]) ]';
  optionsVertex1 = optionsVertex1 || {};
  optionsVertex2 = optionsVertex2 || {};
  var bindVars = {
    "graphName": this.__name,
    "options1": optionsVertex1,
    "options2": optionsVertex2,
    "ex1": ex1,
    "ex2": ex2
  };
  var result = db._query(query, bindVars, {count: true}).toArray(),
    tmp = {}, tmp2={}, returnHash = [];
  result.forEach(function (r) {
    if (!tmp[r[0]]) {
      tmp[r[0]] = [];
    }
    tmp2 = {};
    tmp2[r[1]] = r[2];
    tmp[r[0]].push(tmp2);
  });
  Object.keys(tmp).forEach(function(w) {
    tmp2 = {};
    tmp2[w] = tmp[w];
    returnHash.push(tmp2);
  });
  return returnHash;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_common_properties
/// `graph._commonProperties(vertex1Example, vertex2Examples, options)`
/// *Get the vertices of the graph that share common properties.*
///
/// The function accepts an id, an example, a list of examples or even an empty
/// example as parameter for vertex1Example and vertex2Example.
///
/// The complexity of this method is **O(n)** with *n* being the maximal amount of vertices
/// defined by the parameters vertexExamples.
///
/// *Parameter*
///
/// * *vertex1Examples*: Filter the set of source vertices, see [Definition of examples](#definition_of_examples)
/// * *vertex2Examples*: Filter the set of vertices compared to, see [Definition of examples](#definition_of_examples)
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *vertex1CollectionRestriction* : One or a list of vertex-collection names that should be
///       searched for source vertices.
///   * *vertex2CollectionRestriction* : One or a list of vertex-collection names that should be
///       searched for compare vertices.
///   * *ignoreProperties* : One or a list of attribute names of a document that should be ignored.
///
/// *Examples*
///
/// A route planner example, all locations with the same properties:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleProperties1}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._commonProperties({}, {});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, all cities which share same properties except for population.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleProperties2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._commonProperties({}, {}, {ignoreProperties: 'population'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._commonProperties = function(vertex1Example, vertex2Example, options) {

  var ex1 = transformExample(vertex1Example);
  var ex2 = transformExample(vertex2Example);
  var query = "FOR e"
    + " IN GRAPH_COMMON_PROPERTIES(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options'
    + ')  SORT  ATTRIBUTES(e)[0] RETURN e';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1,
    "ex2": ex2
  };
  return db._query(query, bindVars, {count: true}).toArray();

};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_count_common_properties
/// `graph._countCommonProperties(vertex1Example, vertex2Examples, options)`
/// *Get the amount of vertices of the graph that share common properties.*
///
/// Similar to [_commonProperties](#_commonProperties) but returns count instead of
/// the objects.
///
/// *Examples*
///
/// A route planner example, all locations with the same properties:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAmountProperties1}
/// ~ var db = require("internal").db;
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// graph._countCommonProperties({}, {});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, all German cities which share same properties except for population.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAmountProperties2}
/// ~ var db = require("internal").db;
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// | graph._countCommonProperties({}, {}, {vertex1CollectionRestriction : 'germanCity',
///   vertex2CollectionRestriction : 'germanCity' ,ignoreProperties: 'population'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._countCommonProperties = function(vertex1Example, vertex2Example, options) {
  var ex1 = transformExample(vertex1Example);
  var ex2 = transformExample(vertex2Example);
  var query = "FOR e"
    + " IN GRAPH_COMMON_PROPERTIES(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options'
    + ') FOR a in ATTRIBUTES(e)  SORT  ATTRIBUTES(e)[0] RETURN [ ATTRIBUTES(e)[0], LENGTH(e[a]) ]';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1,
    "ex2": ex2
  };
  var result = db._query(query, bindVars, {count: true}).toArray(), returnHash = [];
  result.forEach(function (r) {
    var tmp = {};
    tmp[r[0]] = r[1];
    returnHash.push(tmp);
  });
  return returnHash;
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_paths
///
/// `graph._paths(options)`
/// *The _paths function returns all paths of a graph.*
///
/// This function determines all available paths in a graph.
///
/// The complexity of this method is **O(n\*n\*m)** with *n* being the amount of vertices in
/// the graph and *m* the average amount of connected edges;
///
/// *Parameters*
///
/// * *options* (optional)    : An object containing options, see below:
///   * *direction*        : The direction of the edges. Possible values are *any*,
/// *inbound* and *outbound* (default).
///   * *followCycles* (optional) : If set to *true* the query follows cycles in the graph,
/// default is false.
///   * *minLength* (optional)     : Defines the minimal length a path must
/// have to be returned (default is 0).
///   * *maxLength* (optional)     : Defines the maximal length a path must
/// have to be returned (default is 10).
///
/// *Examples*
///
/// Return all paths of the graph "social":
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModulePaths}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("social");
///   g._paths();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// Return all inbound paths of the graph "social" with a maximal
/// length of 1 and a minimal length of 2:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModulePaths2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("social");
///   g._paths({direction : 'inbound', minLength : 1, maxLength :  2});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._paths = function(options) {
  var query = "RETURN"
    + " GRAPH_PATHS(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_shortest_path
///
/// `graph._shortestPath(startVertexExample, endVertexExample, options)`
/// *The _shortestPath function returns all shortest paths of a graph.*
///
/// This function determines all shortest paths in a graph.
/// The function accepts an id, an example, a list of examples
/// or even an empty example as parameter for
/// start and end vertex. If one wants to call this function to receive nearly all
/// shortest paths for a graph the option *algorithm* should be set to
/// [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm)
/// to increase performance.
/// If no algorithm is provided in the options the function chooses the appropriate
/// one (either [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm)
/// or [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm)) according to its parameters.
/// The length of a path is by default the amount of edges from one start vertex to
/// an end vertex. The option weight allows the user to define an edge attribute
/// representing the length.
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameters*
///
/// * *startVertexExample* (optional) : An example for the desired start Vertices
/// (see [Definition of examples](#definition_of_examples)).
/// * *endVertexExample* (optional)  : An example for the desired
/// end Vertices (see [Definition of examples](#definition_of_examples)).
/// * *options* (optional) : An object containing options, see below:
///   * *direction*                        : The direction of the edges as a string.
///   Possible values are *outbound*, *inbound* and *any* (default).
///   * *edgeCollectionRestriction*        : One or multiple edge
///   collection names. Only edges from these collections will be considered for the path.
///   * *startVertexCollectionRestriction* : One or multiple vertex
///   collection names. Only vertices from these collections will be considered as
///   start vertex of a path.
///   * *endVertexCollectionRestriction*   : One or multiple vertex
///   collection names. Only vertices from these collections will be considered as
///   end vertex of a path.
///   * *edgeExamples*                     : A filter example for the
///   edges in the shortest paths
///   (see [example](#short_explaination_of_the_vertex_example_parameter)).
///   * *algorithm*                        : The algorithm to calculate
///   the shortest paths. If both start and end vertex examples are empty
///   [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm) is
///   used, otherwise the default is [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm)
///   * *weight*                           : The name of the attribute of
///   the edges containing the length as a string.
///   * *defaultWeight*                    : Only used with the option *weight*.
///   If an edge does not have the attribute named as defined in option *weight* this default
///   is used as length.
///   If no default is supplied the default would be positive Infinity so the path could
///   not be calculated.
///
/// *Examples*
///
/// A route planner example, shortest path from all german to all french cities:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleShortestPaths1}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("routeplanner");
/// | g._shortestPath({}, {}, {weight : 'distance', endVertexCollectionRestriction : 'frenchCity',
///   startVertexCollectionRestriction : 'germanCity'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, shortest path from Hamburg and Cologne to Lyon:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleShortestPaths2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("routeplanner");
/// | g._shortestPath([{_id: 'germanCity/Cologne'},{_id: 'germanCity/Munich'}], 'frenchCity/Lyon',
///   {weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._shortestPath = function(startVertexExample, endVertexExample, options) {
  var ex1 = transformExample(startVertexExample);
  var ex2 = transformExample(endVertexExample);
  var query = "RETURN"
    + " GRAPH_SHORTEST_PATH(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1,
    "ex2": ex2
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_distance_to
///
/// `graph._distanceTo(startVertexExample, endVertexExample, options)`
/// *The _distanceTo function returns all paths and there distance within a graph.*
///
/// This function is a wrapper of [graph._shortestPath](#_shortestpath).
/// It does not return the actual path but only the distance between two vertices.
///
/// *Examples*
///
/// A route planner example, shortest distance from all german to all french cities:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleDistanceTo1}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("routeplanner");
/// | g._distanceTo({}, {}, {weight : 'distance', endVertexCollectionRestriction : 'frenchCity',
///   startVertexCollectionRestriction : 'germanCity'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, shortest distance from Hamburg and Cologne to Lyon:
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleDistanceTo2}
/// ~ var db = require("internal").db;
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var g = examples.loadGraph("routeplanner");
/// | g._distanceTo([{_id: 'germanCity/Cologne'},{_id: 'germanCity/Munich'}], 'frenchCity/Lyon',
///   {weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._distanceTo = function(startVertexExample, endVertexExample, options) {
  var ex1 = transformExample(startVertexExample);
  var ex2 = transformExample(endVertexExample);
  var query = "RETURN"
    + " GRAPH_DISTANCE_TO(@graphName"
    + ',@ex1'
    + ',@ex2'
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1,
    "ex2": ex2
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_absolute_eccentricity
/// 
/// Get the [eccentricity](http://en.wikipedia.org/wiki/Distance_%28graph_theory%29)
/// of the vertices defined by the examples.
///
/// `graph._absoluteEccentricity(vertexExample, options)`
///
/// The function accepts an id, an example, a list of examples or even an empty
/// example as parameter for vertexExample.
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameter*
///
/// * *vertexExample*: Filter the vertices, see [Definition of examples](#definition_of_examples)
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *edgeCollectionRestriction* : One or a list of edge-collection names that should be
///       considered to be on the path.
///   * *startVertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered for source vertices.
///   * *endVertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered for target vertices.
///   * *edgeExamples*: Filter the edges to be followed, see [Definition of examples](#definition_of_examples)
///   * *algorithm*: The algorithm to calculate the shortest paths, possible values are
///        [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm) and
///  [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm).
///   * *weight*: The name of the attribute of the edges containing the weight.
///   * *defaultWeight*: Only used with the option *weight*.
///       If an edge does not have the attribute named as defined in option *weight* this default
///       is used as weight.
///       If no default is supplied the default would be positive infinity so the path and
///       hence the eccentricity can not be calculated.
///
/// @EXAMPLES
///
/// A route planner example, the absolute eccentricity of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsEccentricity1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
/// | db._query("RETURN GRAPH_ABSOLUTE_ECCENTRICITY("
/// |   + "'routeplanner', {})"
///   ).toArray();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute eccentricity of all locations.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsEccentricity2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteEccentricity({}, {weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute eccentricity of all cities regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsEccentricity3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
/// | graph._absoluteEccentricity({}, {startVertexCollectionRestriction : 'germanCity',
///   direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._absoluteEccentricity = function(vertexExample, options) {
  var ex1 = transformExample(vertexExample);
  var query = "RETURN"
    + " GRAPH_ABSOLUTE_ECCENTRICITY(@graphName"
    + ',@ex1'
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_eccentricity
/// `graph._eccentricity(vertexExample, options)`
/// *Get the normalized*
/// [eccentricity](http://en.wikipedia.org/wiki/Distance_%28graph_theory%29)
/// *of the vertices defined by the examples.*
///
/// Similar to [_absoluteEccentricity](#_absoluteeccentricity) but returns a normalized result.
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// @EXAMPLES
///
/// A route planner example, the eccentricity of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleEccentricity2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._eccentricity();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the weighted eccentricity.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleEccentricity3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._eccentricity({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._eccentricity = function(options) {
  var query = "RETURN"
    + " GRAPH_ECCENTRICITY(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};



////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_absolute_closeness
/// `graph._absoluteCloseness(vertexExample, options)`
/// *Get the*
/// [closeness](http://en.wikipedia.org/wiki/Centrality#Closeness_centrality)
/// *of the vertices defined by the examples.*
///
/// The function accepts an id, an example, a list of examples or even an empty
/// example as parameter for *vertexExample*.
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameter*
///
/// * *vertexExample*: Filter the vertices, see [Definition of examples](#definition_of_examples)
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *edgeCollectionRestriction* : One or a list of edge-collection names that should be
///       considered to be on the path.
///   * *startVertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered for source vertices.
///   * *endVertexCollectionRestriction* : One or a list of vertex-collection names that should be
///       considered for target vertices.
///   * *edgeExamples*: Filter the edges to be followed, see [Definition of examples](#definition_of_examples)
///   * *algorithm*: The algorithm to calculate the shortest paths, possible values are
///        [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm) and
///  [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm).
///   * *weight*: The name of the attribute of the edges containing the weight.
///   * *defaultWeight*: Only used with the option *weight*.
///       If an edge does not have the attribute named as defined in option *weight* this default
///       is used as weight.
///       If no default is supplied the default would be positive infinity so the path and
///       hence the closeness can not be calculated.
///
/// @EXAMPLES
///
/// A route planner example, the absolute closeness of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsCloseness1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteCloseness({});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute closeness of all locations.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsCloseness2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteCloseness({}, {weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute closeness of all German Cities regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsCloseness3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
/// | graph._absoluteCloseness({}, {startVertexCollectionRestriction : 'germanCity',
///   direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._absoluteCloseness = function(vertexExample, options) {
  var ex1 = transformExample(vertexExample);
  var query = "RETURN"
    + " GRAPH_ABSOLUTE_CLOSENESS(@graphName"
    + ',@ex1'
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options,
    "ex1": ex1
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_closeness
/// `graph._closeness(options)`
/// *Get the normalized*
/// [closeness](http://en.wikipedia.org/wiki/Centrality#Closeness_centrality)
/// *of graphs vertices.*
///
/// Similar to [_absoluteCloseness](#_absolutecloseness) but returns a normalized value.
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// @EXAMPLES
///
/// A route planner example, the normalized closeness of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCloseness1}
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// graph._closeness();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the closeness of all locations.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCloseness2}
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// graph._closeness({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the closeness of all cities regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleCloseness3}
/// var examples = require("org/arangodb/graph-examples/example-graph.js");
/// var graph = examples.loadGraph("routeplanner");
/// graph._closeness({direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._closeness = function(options) {
  var query = "RETURN"
    + " GRAPH_CLOSENESS(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};



////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_absolute_betweenness
///
/// `graph._absoluteBetweenness(options)`
/// *Get the*
/// [betweenness](http://en.wikipedia.org/wiki/Betweenness_centrality)
/// *of all vertices in the graph.*
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameter*
///
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *weight*: The name of the attribute of the edges containing the weight.
///   * *defaultWeight*: Only used with the option *weight*.
///       If an edge does not have the attribute named as defined in option *weight* this default
///       is used as weight.
///       If no default is supplied the default would be positive infinity so the path and
///       hence the betweeness can not be calculated.
///
/// @EXAMPLES
///
/// A route planner example, the absolute betweenness of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsBetweenness1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteBetweenness({});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute betweenness of all locations.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsBetweenness2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteBetweenness({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the absolute betweenness of all cities regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleAbsBetweenness3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._absoluteBetweenness({direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._absoluteBetweenness = function(options) {

  var query = "RETURN"
    + " GRAPH_ABSOLUTE_BETWEENNESS(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_betweenness
///
/// `graph_module._betweenness(options)`
/// *Get the normalized*
/// [betweenness](http://en.wikipedia.org/wiki/Betweenness_centrality)
/// *of graphs vertices.*
///
/// Similar to [_absoluteBetweeness](#_absolutebetweeness) but returns normalized values.
///
/// @EXAMPLES
///
/// A route planner example, the betweenness of all locations.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleBetweenness1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._betweenness();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the betweenness of all locations.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleBetweenness2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._betweenness({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the betweenness of all cities regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleBetweenness3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._betweenness({direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._betweenness = function(options) {

  var query = "RETURN"
    + " GRAPH_BETWEENNESS(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};



////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_radius
/// `graph._radius(options)`
/// *Get the*
/// [radius](http://en.wikipedia.org/wiki/Eccentricity_%28graph_theory%29)
/// *of a graph.*
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameter*
///
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *algorithm*: The algorithm to calculate the shortest paths, possible values are
///        [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm) and
///  [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm).
///   * *weight*: The name of the attribute of the edges containing the weight.
///   * *defaultWeight*: Only used with the option *weight*.
///       If an edge does not have the attribute named as defined in option *weight* this default
///       is used as weight.
///       If no default is supplied the default would be positive infinity so the path and
///       hence the radius can not be calculated.
///
/// @EXAMPLES
///
/// A route planner example, the radius of the graph.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleRadius1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._radius();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the radius of the graph.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleRadius2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._radius({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the radius of the graph regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleRadius3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._radius({direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
//
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._radius = function(options) {

  var query = "RETURN"
    + " GRAPH_RADIUS(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};



////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_diameter
/// `graph._diameter(graphName, options)`
/// *Get the*
/// [diameter](http://en.wikipedia.org/wiki/Eccentricity_%28graph_theory%29)
/// *of a graph.*
///
/// The complexity of the function is described
/// [here](../Aql/GraphOperations.html#the_complexity_of_the_shortest_path_algorithms).
///
/// *Parameter*
///
/// * *options* (optional): An object defining further options. Can have the following values: 
///   * *direction*: The direction of the edges. Possible values are *outbound*, *inbound* and *any* (default).
///   * *algorithm*: The algorithm to calculate the shortest paths, possible values are
///        [Floyd-Warshall](http://en.wikipedia.org/wiki/Floyd%E2%80%93Warshall_algorithm) and
///  [Dijkstra](http://en.wikipedia.org/wiki/Dijkstra's_algorithm).
///   * *weight*: The name of the attribute of the edges containing the weight.
///   * *defaultWeight*: Only used with the option *weight*.
///       If an edge does not have the attribute named as defined in option *weight* this default
///       is used as weight.
///       If no default is supplied the default would be positive infinity so the path and
///       hence the radius can not be calculated.
///
/// @EXAMPLES
///
/// A route planner example, the diameter of the graph.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleDiameter1}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._diameter();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the diameter of the graph.
/// This considers the actual distances.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleDiameter2}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._diameter({weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// A route planner example, the diameter of the graph regarding only
/// outbound paths.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{generalGraphModuleDiameter3}
///   var examples = require("org/arangodb/graph-examples/example-graph.js");
///   var graph = examples.loadGraph("routeplanner");
///   graph._diameter({direction : 'outbound', weight : 'distance'});
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._diameter = function(options) {

  var query = "RETURN"
    + " GRAPH_DIAMETER(@graphName"
    + ',@options'
    + ')';
  options = options || {};
  var bindVars = {
    "graphName": this.__name,
    "options": options
  };
  var result = db._query(query, bindVars).toArray();
  return result;
};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__extendEdgeDefinitions
/// `graph._extendEdgeDefinitions(edgeDefinition)`
/// *Add another edge definition to the graph*
///
/// Extends the edge definitions of a graph. If an orphan collection is used in this
/// edge definition, it will be removed from the orphanage. If the edge collection of
/// the edge definition to add is already used in the graph or used in a different
/// graph with different *from* and/or *to* collections an error is thrown.
///
/// *Parameter*
///
/// * *edgeDefinition*: The relation definition to extend the graph
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__extendEdgeDefinitions}
///   var graph_module = require("org/arangodb/general-graph")
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var ed2 = graph_module._directedRelation("myEC2", ["myVC1"], ["myVC3"]);
///   var graph = graph_module._create("myGraph", [ed1]);
///   graph._extendEdgeDefinitions(ed2);
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._extendEdgeDefinitions = function(edgeDefinition) {
  edgeDefinition = sortEdgeDefinition(edgeDefinition);
  var self = this;
  var err;
  //check if edgeCollection not already used
  var eC = edgeDefinition.collection;
  // ... in same graph
  if (this.__edgeCollections[eC]  !== undefined) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_COLLECTION_MULTI_USE.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_COLLECTION_MULTI_USE.message;
    throw err;
  }
  //in different graph
  db._graphs.toArray().forEach(
    function(singleGraph) {
      var sGEDs = singleGraph.edgeDefinitions;
      sGEDs.forEach(
        function(sGED) {
          var col = sGED.collection;
          if (col === eC) {
            if (JSON.stringify(sGED) !== JSON.stringify(edgeDefinition)) {
              err = new ArangoError();
              err.errorNum = arangodb.errors.ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS.code;
              err.errorMessage = col
                + " " + arangodb.errors.ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS.message;
              throw err;
            }
          }
        }
      );
    }
  );

  findOrCreateCollectionsByEdgeDefinitions([edgeDefinition]);

  this.__edgeDefinitions.push(edgeDefinition);
  db._graphs.update(this.__name, {edgeDefinitions: this.__edgeDefinitions});
  this.__edgeCollections[edgeDefinition.collection] = db[edgeDefinition.collection];
  edgeDefinition.from.forEach(
    function(vc) {
      self[vc] = db[vc];
      //remove from __orphanCollections
      var orphanIndex = self.__orphanCollections.indexOf(vc);
      if (orphanIndex !== -1) {
        self.__orphanCollections.splice(orphanIndex, 1);
      }
      //push into __vertexCollections
      if (self.__vertexCollections[vc] === undefined) {
        self.__vertexCollections[vc] = db[vc];
      }
    }
  );
  edgeDefinition.to.forEach(
    function(vc) {
      self[vc] = db[vc];
      //remove from __orphanCollections
      var orphanIndex = self.__orphanCollections.indexOf(vc);
      if (orphanIndex !== -1) {
        self.__orphanCollections.splice(orphanIndex, 1);
      }
      //push into __vertexCollections
      if (self.__vertexCollections[vc] === undefined) {
        self.__vertexCollections[vc] = db[vc];
      }
    }
  );
  updateBindCollections(this);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function for editing edge definitions
////////////////////////////////////////////////////////////////////////////////

var changeEdgeDefinitionsForGraph = function(graph, edgeDefinition, newCollections, possibleOrphans, self) {

  var oldCollections = [];
  var graphCollections = [];
  var graphObj = _graph(graph._key);
  var eDs = graph.edgeDefinitions;
  var gotAHit = false;

  //replace edgeDefintion
  eDs.forEach(
    function(eD, id) {
      if(eD.collection === edgeDefinition.collection) {
        gotAHit = true;
        oldCollections = _.union(oldCollections, eD.from);
        oldCollections = _.union(oldCollections, eD.to);
        eDs[id].from = edgeDefinition.from;
        eDs[id].to = edgeDefinition.to;
        db._graphs.update(graph._key, {edgeDefinitions: eDs});
        if (graph._key === self.__name) {
          self.__edgeDefinitions[id].from = edgeDefinition.from;
          self.__edgeDefinitions[id].to = edgeDefinition.to;
        }
      } else {
        //collect all used collections
        graphCollections = _.union(graphCollections, eD.from);
        graphCollections = _.union(graphCollections, eD.to);
      }
    }
  );


  //remove used collection from orphanage
  newCollections.forEach(
    function(nc) {
      if (graph._key === self.__name) {
        if (self.__vertexCollections[nc] === undefined) {
          self.__vertexCollections[nc] = db[nc];
        }
        try {
          graphObj._removeVertexCollection(nc, false);
        } catch (ignore) {
        }
      }
    }
  );

  //move unused collections to orphanage
  possibleOrphans.forEach(
    function(po) {
      if (graphCollections.indexOf(po) === -1 && gotAHit) {
        delete graphObj.__vertexCollections[po];
        graphObj._addVertexCollection(po);
      }
    }
  );
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__editEdgeDefinition
/// `general-graph._editEdgeDefinition(edgeDefinition)`
/// *Modify an relation definition*
///
/// Edits one relation definition of a graph. The edge definition used as argument will
/// replace the existing edge definition of the graph which has the same collection.
/// Vertex Collections of the replaced edge definition that are not used in the new
/// definition will transform to an orphan. Orphans that are used in this new edge
/// definition will be deleted from the list of orphans. Other graphs with the same edge
/// definition will be modified, too.
///
/// *Parameter*
///
/// * *edgeDefinition*: The edge definition to replace the existing edge
/// definition with the same attribute *collection*.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__editEdgeDefinition}
///   var graph_module = require("org/arangodb/general-graph")
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var ed2 = graph_module._directedRelation("myEC1", ["myVC2"], ["myVC3"]);
///   var graph = graph_module._create("myGraph", [ed1, ed2]);
///   graph._editEdgeDefinition(ed2, true);
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
Graph.prototype._editEdgeDefinitions = function(edgeDefinition) {
  edgeDefinition = sortEdgeDefinition(edgeDefinition);
  var self = this;

  //check, if in graphs edge definition
  if (this.__edgeCollections[edgeDefinition.collection] === undefined) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_EDGE_COLLECTION_NOT_USED.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_EDGE_COLLECTION_NOT_USED.message;
    throw err;
  }

  findOrCreateCollectionsByEdgeDefinitions([edgeDefinition]);

  //evaluate collections to add to orphanage
  var possibleOrphans = [];
  var currentEdgeDefinition;
  this.__edgeDefinitions.forEach(
    function(ed) {
      if (edgeDefinition.collection === ed.collection) {
        currentEdgeDefinition = ed;
      }
    }
  );

  var currentCollections = _.union(currentEdgeDefinition.from, currentEdgeDefinition.to);
  var newCollections = _.union(edgeDefinition.from, edgeDefinition.to);
  currentCollections.forEach(
    function(colName) {
      if (newCollections.indexOf(colName) === -1) {
        possibleOrphans.push(colName);
      }
    }
  );
  //change definition for ALL graphs
  var graphs = getGraphCollection().toArray();
  graphs.forEach(
    function(graph) {
      changeEdgeDefinitionsForGraph(graph, edgeDefinition, newCollections, possibleOrphans, self);
    }
  );
  updateBindCollections(this);

};


////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__deleteEdgeDefinition
/// `general-graph._deleteEdgeDefinition(edgeCollectionName)`
/// *Delete one relation definition*
///
/// Deletes a relation definition defined by the edge collection of a graph. If the
/// collections defined in the edge definition (collection, from, to) are not used
/// in another edge definition of the graph, they will be moved to the orphanage.
///
/// *Parameter*
///
/// * *edgeCollectionName*: Name of edge collection in the relation definition.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__deleteEdgeDefinition}
///   var graph_module = require("org/arangodb/general-graph")
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var ed2 = graph_module._directedRelation("myEC2", ["myVC1"], ["myVC3"]);
///   var graph = graph_module._create("myGraph", [ed1, ed2]);
///   graph._deleteEdgeDefinition("myEC1");
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._deleteEdgeDefinition = function(edgeCollection) {

  //check, if in graphs edge definition
  if (this.__edgeCollections[edgeCollection] === undefined) {
    var err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_EDGE_COLLECTION_NOT_USED.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_EDGE_COLLECTION_NOT_USED.message;
    throw err;
  }

  var edgeDefinitions = this.__edgeDefinitions,
    self = this,
    usedVertexCollections = [],
    possibleOrphans = [],
    index;

  edgeDefinitions.forEach(
    function(edgeDefinition, idx) {
      if (edgeDefinition.collection === edgeCollection) {
        index = idx;
        possibleOrphans = edgeDefinition.from;
        possibleOrphans = _.union(possibleOrphans, edgeDefinition.to);
      } else {
        usedVertexCollections = _.union(usedVertexCollections, edgeDefinition.from);
        usedVertexCollections = _.union(usedVertexCollections, edgeDefinition.to);
      }
    }
  );
  this.__edgeDefinitions.splice(index, 1);
  possibleOrphans.forEach(
    function(po) {
      if (usedVertexCollections.indexOf(po) === -1) {
        self.__orphanCollections.push(po);
      }
    }
  );

  updateBindCollections(this);
  db._graphs.update(
    this.__name,
    {
      orphanCollections: this.__orphanCollections,
      edgeDefinitions: this.__edgeDefinitions
    }
  );


};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__addVertexCollection
/// `graph._addVertexCollection(vertexCollectionName, createCollection)`
/// *Add a vertex collection to the graph*
///
/// Adds a vertex collection to the set of orphan collections of the graph. If the
/// collection does not exist, it will be created. If it is already used by any edge
/// definition of the graph, an error will be thrown.
///
/// *Parameter*
///
/// * *vertexCollectionName*: Name of vertex collection.
/// * *createCollection* (optional): If true the collection will be created if it does not exist. Default: true.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__addVertexCollection}
///   var graph_module = require("org/arangodb/general-graph");
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var graph = graph_module._create("myGraph", [ed1]);
///   graph._addVertexCollection("myVC3", true);
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._addVertexCollection = function(vertexCollectionName, createCollection) {
  //check edgeCollection
  var ec = db._collection(vertexCollectionName);
  var err;
  if (ec === null) {
    if (createCollection !== false) {
      db._create(vertexCollectionName);
    } else {
      err = new ArangoError();
      err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
      err.errorMessage = vertexCollectionName + arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message;
      throw err;
    }
  } else if (ec.type() !== 2) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_WRONG_COLLECTION_TYPE_VERTEX.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_WRONG_COLLECTION_TYPE_VERTEX.message;
    throw err;
  }
  if (this.__vertexCollections[vertexCollectionName] !== undefined) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_COLLECTION_USED_IN_EDGE_DEF.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_COLLECTION_USED_IN_EDGE_DEF.message;
    throw err;
  }

  this.__orphanCollections.push(vertexCollectionName);
  this[vertexCollectionName] = db[vertexCollectionName];
  updateBindCollections(this);
  db._graphs.update(this.__name, {orphanCollections: this.__orphanCollections});

};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__orphanCollections
/// `graph._orphanCollections()`
/// *Get all orphan collections*
///
/// Returns all vertex collections of the graph that are not used in any edge definition.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__orphanCollections}
///   var graph_module = require("org/arangodb/general-graph")
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var graph = graph_module._create("myGraph", [ed1]);
///   graph._addVertexCollection("myVC3", true);
///   graph._orphanCollections();
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._orphanCollections = function() {
  return this.__orphanCollections;
};

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph__removeVertexCollection
/// `graph._removeVertexCollection(vertexCollectionName, dropCollection)`
/// *Remove a vertex collection from the graph*
///
/// Removes a vertex collection from the graph.
/// Only collections not used in any relation definition can be removed.
/// Optionally the collection can be deleted, if it is not used in any other graph.
///
/// *Parameter*
///
/// * *vertexCollectionName*: Name of vertex collection.
/// * *dropCollection* (optional): If true the collection will be dropped if it is
///     not used in any other graph. Default: false.
///
/// *Examples*
///
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph__removeVertexCollections}
///   var graph_module = require("org/arangodb/general-graph")
/// ~ if (graph_module._exists("myGraph")){var blub = graph_module._drop("myGraph", true);}
///   var ed1 = graph_module._directedRelation("myEC1", ["myVC1"], ["myVC2"]);
///   var graph = graph_module._create("myGraph", [ed1]);
///   graph._addVertexCollection("myVC3", true);
///   graph._addVertexCollection("myVC4", true);
///   graph._orphanCollections();
///   graph._removeVertexCollection("myVC3");
///   graph._orphanCollections();
/// ~ var blub = graph_module._drop("myGraph", true);
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._removeVertexCollection = function(vertexCollectionName, dropCollection) {
  var err;
  if (db._collection(vertexCollectionName) === null) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST.message;
    throw err;
  }
  var index = this.__orphanCollections.indexOf(vertexCollectionName);
  if (index === -1) {
    err = new ArangoError();
    err.errorNum = arangodb.errors.ERROR_GRAPH_NOT_IN_ORPHAN_COLLECTION.code;
    err.errorMessage = arangodb.errors.ERROR_GRAPH_NOT_IN_ORPHAN_COLLECTION.message;
    throw err;
  }
  this.__orphanCollections.splice(index, 1);
  delete this[vertexCollectionName];
  db._graphs.update(this.__name, {orphanCollections: this.__orphanCollections});

  if (dropCollection === true) {
    var graphs = getGraphCollection().toArray();
    if (checkIfMayBeDropped(vertexCollectionName, null, graphs)) {
      db._drop(vertexCollectionName);
    }
  }
  updateBindCollections(this);
};




////////////////////////////////////////////////////////////////////////////////
/// @brief print basic information for the graph
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._PRINT = function(context) {
  var name = this.__name;
  var edgeDefs = printEdgeDefinitions(this.__edgeDefinitions);
  context.output += "[ Graph ";
  context.output += name;
  context.output += " EdgeDefinitions: ";
  internal.printRecursive(edgeDefs, context);
  context.output += " VertexCollections: ";
  internal.printRecursive(this.__orphanCollections, context);
  context.output += " ]";
};

// -----------------------------------------------------------------------------
// --SECTION--                                                    MODULE EXPORTS
// -----------------------------------------------------------------------------

exports._undirectedRelation = _undirectedRelation;
exports._directedRelation = _directedRelation;
exports._graph = _graph;
exports._edgeDefinitions = _edgeDefinitions;
exports._extendEdgeDefinitions = _extendEdgeDefinitions;
exports._create = _create;
exports._drop = _drop;
exports._exists = _exists;
exports._list = _list;
exports._listObjects = _listObjects;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// @addtogroup\\|// --SECTION--\\|/// @page\\|/// @}\\)"
// End:


////////////////////////////////////////////////////////////////////////////////
/// some more documentation
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_create_graph_example1
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph_create_graph_example1}
///   var graph_module = require("org/arangodb/general-graph");
///   var edgeDefinitions = graph_module._edgeDefinitions();
///   graph_module._extendEdgeDefinitions(edgeDefinitions, graph_module._undirectedRelation("friend_of", ["Customer"]));
/// | graph_module._extendEdgeDefinitions(
/// | edgeDefinitions, graph_module._directedRelation(
///   "has_bought", ["Customer", "Company"], ["Groceries", "Electronics"]));
///   graph_module._create("myStore", edgeDefinitions);
/// ~ graph_module._drop("myStore");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_general_graph_create_graph_example2
/// @EXAMPLE_ARANGOSH_OUTPUT{general_graph_create_graph_example2}
///   var graph_module = require("org/arangodb/general-graph");
/// |  var edgeDefinitions = graph_module._edgeDefinitions(
/// |  graph_module._undirectedRelation("friend_of", ["Customer"]), graph_module._directedRelation(
///    "has_bought", ["Customer", "Company"], ["Groceries", "Electronics"]));
///   graph_module._create("myStore", edgeDefinitions);
/// ~ graph_module._drop("myStore");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
///
////////////////////////////////////////////////////////////////////////////////
});
