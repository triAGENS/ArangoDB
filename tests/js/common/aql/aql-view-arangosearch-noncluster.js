/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertUndefined, assertEqual, assertTrue, assertFalse, fail*/

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for iresearch usage
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
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var db = require("@arangodb").db;
var ERRORS = require("@arangodb").errors;
var internal = require('internal');
const isServer = typeof arango === 'undefined';

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function iResearchAqlTestSuite () {
  var c;
  var v;

  return {
    setUp : function () {
      db._drop("UnitTestsCollection");
      c = db._create("UnitTestsCollection");

      db._drop("AnotherUnitTestsCollection");
      var ac = db._create("AnotherUnitTestsCollection");

      db._dropView("UnitTestsView");
      v = db._createView("UnitTestsView", "arangosearch", {});
      var meta = {
        links: { 
          "UnitTestsCollection": { 
            includeAllFields: true,
            storeValues: "id",
            fields: {
              text: { analyzers: [ "text_en" ] }
            }
          }
        }
      };
      v.properties(meta);

      ac.save({ a: "foo", id : 0 });
      ac.save({ a: "ba", id : 1 });

      for (let i = 0; i < 5; i++) {
        c.save({ a: "foo", b: "bar", c: i });
        c.save({ a: "foo", b: "baz", c: i });
        c.save({ a: "bar", b: "foo", c: i });
        c.save({ a: "baz", b: "foo", c: i });
      }

      c.save({ name: "full", text: "the quick brown fox jumps over the lazy dog" });
      c.save({ name: "half", text: "quick fox over lazy" });
      c.save({ name: "other half", text: "the brown jumps the dog" });
      c.save({ name: "quarter", text: "quick over" });

      c.save({ name: "numeric", anotherNumericField: 0 });
      c.save({ name: "null", anotherNullField: null });
      c.save({ name: "bool", anotherBoolField: true });
      c.save({ _key: "foo", xyz: 1 });
    },

    tearDown : function () {
      var meta = { links : { "UnitTestsCollection": null } };
      v.properties(meta);
      v.drop();
      db._drop("UnitTestsCollection");
      db._drop("AnotherUnitTestsCollection");
    },

    testViewInFunctionCall : function () {
      try {
        db._query("FOR doc IN 1..1 RETURN COUNT(UnitTestsView)");
      } catch (e) {
        assertEqual(ERRORS.ERROR_NOT_IMPLEMENTED.code, e.errorNum);
      }
    },

    testTransactionRegistration : function () {
      // read lock
      var result = db._executeTransaction({
        collections: {
          allowImplicit: false,
          read: [ v.name() ]
        },
        action: function () {
          var db = require("@arangodb").db;
          var c = db._collection("UnitTestsCollection");
          return {length: c.toArray().length, value: c.document('foo').xyz};
        }
      });
      assertEqual(1, result.value);
      assertEqual(28, result.length);

      // write lock
      result = db._executeTransaction({
        collections: {
          allowImplicit: false,
          write: [ v.name() ]
        },
        action: function () {
          var db = require("@arangodb").db;
          var c = db._collection("UnitTestsCollection");
          c.save({ _key: "bar", xyz: 2 });
          return c.toArray().length;
        }
      });
      assertEqual(29, result);

      // exclusive lock
      result = db._executeTransaction({
        collections: {
          allowImplicit: false,
          exclusive: [ v.name() ]
        },
        action: function () {
          var db = require("@arangodb").db;
          var c = db._collection("UnitTestsCollection");
          c.save({ _key: "baz", xyz: 3 });
          return c.toArray().length;
        }
      });
      assertEqual(30, result);
    },

    testAttributeEqualityFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.a == 'foo' OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 10);
      result.forEach(function(res) {
        assertEqual(res.a, "foo");
      });
    },

    testMultipleAttributeEqualityFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.a == 'foo' && doc.b == 'bar' OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 5);
      result.forEach(function(res) {
        assertEqual(res.a, "foo");
        assertEqual(res.b, "bar");
      });
    },

    testMultipleAttributeEqualityFilterSortAttribute : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.a == 'foo' && doc.b == 'bar' OPTIONS { waitForSync : true } SORT doc.c RETURN doc").toArray();

      assertEqual(result.length, 5);
      var last = -1;
      result.forEach(function(res) {
        assertEqual(res.a, "foo");
        assertEqual(res.b, "bar");
        assertEqual(res.c, last + 1);
        last = res.c;
      });
    },

    testMultipleAttributeEqualityFilterSortAttributeDesc : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.a == 'foo' AND doc.b == 'bar' OPTIONS { waitForSync : true } SORT doc.c DESC RETURN doc").toArray();

      assertEqual(result.length, 5);
      var last = 5;
      result.forEach(function(res) {
        assertEqual(res.a, "foo");
        assertEqual(res.b, "bar");
        assertEqual(res.c, last - 1);
        last = res.c;
      });
    },

    testAttributeLessFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c < 2 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 8);
      result.forEach(function(res) {
        assertTrue(res.c < 2);
      });
    },

    testAttributeLeqFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c <= 2 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 12);
      result.forEach(function(res) {
        assertTrue(res.c <= 2);
      });
    },

    testAttributeGeqFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c >= 2 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 12);
      result.forEach(function(res) {
        assertTrue(res.c >= 2);
      });
    },

    testAttributeGreaterFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c > 2 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 8);
      result.forEach(function(res) {
        assertTrue(res.c > 2);
      });
    },

    testAttributeOpenIntervalFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c > 1 AND doc.c < 3 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 4);
      result.forEach(function(res) {
        assertTrue(res.c > 1 && res.c < 3);
      });
    },

    testAttributeClosedIntervalFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c >= 1 AND doc.c <= 3 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 12);
      result.forEach(function(res) {
        assertTrue(res.c >= 1 && res.c <= 3);
      });
    },

    testAttributeIntervalExclusionFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c < 1 OR doc.c > 3 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 8);
      result.forEach(function(res) {
        assertTrue(res.c < 1 || res.c > 3);
      });
    },

    testAttributeNeqFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.a != 'foo' OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 18); // include documents without attribute 'a'
      result.forEach(function(res) {
        assertFalse(res.a === 'foo');
      });
    },

    testStartsWithFilter : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc.a, 'fo') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 10);
      result.forEach(function(res) {
        assertEqual(res.a, 'foo');
      });
    },

    testStartsWithFilter2 : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc.b, 'ba') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 10);
      result.forEach(function(res) {
        assertTrue(res.b === 'bar' || res.b === 'baz');
      });
    },

    testStartsWithFilterSort : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc.b, 'ba') && doc.c == 0 OPTIONS { waitForSync : true } SORT doc.b RETURN doc").toArray();

      assertEqual(result.length, 2);
      assertEqual(result[0].b, 'bar');
      assertEqual(result[1].b, 'baz');
      assertEqual(result[0].c, 0);
      assertEqual(result[1].c, 0);
    },

    testInTokensFilterSortTFIDF : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH ANALYZER(doc.text IN TOKENS('the quick brown', 'text_en'), 'text_en') OPTIONS { waitForSync : true } SORT TFIDF(doc) LIMIT 4 RETURN doc").toArray();

      assertEqual(result.length, 4);
      assertEqual(result[0].name, 'half');
      assertEqual(result[1].name, 'quarter');
      assertEqual(result[2].name, 'other half');
      assertEqual(result[3].name, 'full');
    },

    testPhraseFilter : function () {
      var result0 = db._query("FOR doc IN UnitTestsView SEARCH PHRASE(doc.text, 'quick brown fox jumps', 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result0.length, 1);
      assertEqual(result0[0].name, 'full');

      var result1 = db._query("FOR doc IN UnitTestsView SEARCH PHRASE(doc.text, [ 'quick brown fox jumps' ], 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result1.length, 1);
      assertEqual(result1[0].name, 'full');

      var result2 = db._query("FOR doc IN UnitTestsView SEARCH ANALYZER(PHRASE(doc.text, 'quick brown fox jumps'), 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result2.length, 1);
      assertEqual(result2[0].name, 'full');

      var result3 = db._query("FOR doc IN UnitTestsView SEARCH ANALYZER(PHRASE(doc.text, [ 'quick brown fox jumps' ]), 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result3.length, 1);
      assertEqual(result3[0].name, 'full');
    },

    testExistsFilter : function () {
      var expected = new Set();
      expected.add("full");
      expected.add("half");
      expected.add("other half");
      expected.add("quarter");

      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc.text) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size); 
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByAnalyzer : function () {
      var expected = new Set();
      expected.add("full");
      expected.add("half");
      expected.add("other half");
      expected.add("quarter");

      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc.text, 'analyzer', 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByContextAnalyzer: function () {
      var expected = new Set();
      expected.add("full");
      expected.add("half");
      expected.add("other half");
      expected.add("quarter");

      var result = db._query("FOR doc IN UnitTestsView SEARCH ANALYZER(EXISTS(doc.text, 'analyzer'), 'text_en') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByString: function () {
      var expected = new Set();
      expected.add("full");
      expected.add("half");
      expected.add("other half");
      expected.add("quarter");

      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc.text, 'string') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByType : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc.text, 'type') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 0);
    },

    testExistsFilterByTypeNull : function () {
      var expected = new Set();
      expected.add("null");

      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc.anotherNullField, 'null') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByTypeBool : function () {
      var expected = new Set();
      expected.add("bool");

      var result = db._query("FOR doc IN UnitTestsView SEARCH EXISTS(doc['anotherBoolField'], 'bool') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testExistsFilterByTypeNumeric : function () {
      var expected = new Set();
      expected.add("numeric");

      var result = db._query("LET suffix='NumericField' LET fieldName = CONCAT('another', suffix) FOR doc IN UnitTestsView SEARCH EXISTS(doc[fieldName], 'numeric') OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(res.name));
      });
      assertEqual(expected.size, 0);
    },

    testViewInInnerLoop : function() {
      var expected = new Set(); // FIXME is there a better way to compare objects in js?
      expected.add(JSON.stringify({ a: "foo", b: "bar", c: 0 }));
      expected.add(JSON.stringify({ a: "foo", b: "baz", c: 0 }));
      expected.add(JSON.stringify({ a: "bar", b: "foo", c: 1 }));
      expected.add(JSON.stringify({ a: "baz", b: "foo", c: 1 }));

      var result = db._query(
        "FOR adoc IN AnotherUnitTestsCollection " +
        "FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc['a'], adoc.a) && adoc.id == doc.c OPTIONS { waitForSync : true } " +
        "RETURN doc"
      ).toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(JSON.stringify({ a: res.a, b: res.b, c: res.c })));
      });
      assertEqual(expected.size, 0);
    },

    testViewInInnerLoopMultipleFilters : function() {
      var expected = new Set(); // FIXME is there a better way to compare objects in js?
      expected.add(JSON.stringify({ a: "foo", b: "bar", c: 0 }));
      expected.add(JSON.stringify({ a: "foo", b: "baz", c: 0 }));

      var result = db._query(
        "FOR adoc IN AnotherUnitTestsCollection FILTER adoc.id < 1 " +
        "FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc['a'], adoc.a) && adoc.id == doc.c OPTIONS { waitForSync : true } " +
        "RETURN doc"
      ).toArray();

      assertEqual(result.length, expected.size);
      result.forEach(function(res) {
        assertTrue(expected.delete(JSON.stringify({ a: res.a, b: res.b, c: res.c })));
      });
      assertEqual(expected.size, 0);
    },

    testViewInInnerLoopSortByAttribute : function() {
      var expected = [];
      expected.push({ a: "bar", b: "foo", c: 1 });
      expected.push({ a: "baz", b: "foo", c: 1 });
      expected.push({ a: "foo", b: "bar", c: 0 });
      expected.push({ a: "foo", b: "baz", c: 0 });

      var result = db._query(
        "FOR adoc IN AnotherUnitTestsCollection " +
        "FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc['a'], adoc.a) && adoc.id == doc.c OPTIONS { waitForSync : true } " +
        "SORT doc.c DESC, doc.a, doc.b " +
        "RETURN doc"
      ).toArray();

      assertEqual(result.length, expected.length);
      var i = 0;
      result.forEach(function(res) {
        var doc = expected[i++];
        assertEqual(doc.a, res.a);
        assertEqual(doc.b, res.b);
        assertEqual(doc.c, res.c);
      });
    },
	
    testViewInInnerLoopSortByAttributeWithNonDeterministic : function() {
      var expected = [];
      expected.push({ a: "bar", b: "foo", c: 1 });
      expected.push({ a: "baz", b: "foo", c: 1 });
      expected.push({ a: "foo", b: "bar", c: 0 });
      expected.push({ a: "foo", b: "baz", c: 0 });

      var result = db._query(
        "FOR adoc IN AnotherUnitTestsCollection " +
        "FOR doc IN UnitTestsView SEARCH RAND() != -10 && STARTS_WITH(doc['a'], adoc.a) && adoc.id == doc.c OPTIONS { waitForSync : true } " +
        "SORT doc.c DESC, doc.a, doc.b " +
        "RETURN doc"
      ).toArray();

      assertEqual(result.length, expected.length);
      var i = 0;
      result.forEach(function(res) {
        var doc = expected[i++];
        assertEqual(doc.a, res.a);
        assertEqual(doc.b, res.b);
        assertEqual(doc.c, res.c);
      });
    },

    testViewInInnerLoopSortByTFIDF_BM25_Attribute : function() {
      var expected = [];
      expected.push({ a: "baz", b: "foo", c: 1 });
      expected.push({ a: "bar", b: "foo", c: 1 });
      expected.push({ a: "foo", b: "bar", c: 0 });
      expected.push({ a: "foo", b: "baz", c: 0 });

      var result = db._query(
        "FOR adoc IN AnotherUnitTestsCollection " +
        "FOR doc IN UnitTestsView SEARCH STARTS_WITH(doc['a'], adoc.a) && adoc.id == doc.c OPTIONS { waitForSync : true } " +
        "SORT TFIDF(doc) DESC, BM25(doc) DESC, doc.a DESC, doc.b " +
        "RETURN doc"
      ).toArray();

      assertEqual(result.length, expected.length);
      var i = 0;
      result.forEach(function(res) {
        var doc = expected[i++];
        assertEqual(doc.a, res.a);
        assertEqual(doc.b, res.b);
        assertEqual(doc.c, res.c);
      });
    },

    testViewInInnerLoopOptimized : function() {
      var expected = [];
      expected.push({ a: "foo", b: "bar", c: 0 });
      expected.push({ a: "foo", b: "baz", c: 0 });

      var result = db._query("LET outer = (FOR out1 IN UnitTestsCollection FILTER out1.a == 'foo' && out1.c == 0 RETURN out1) FOR a IN outer FOR d IN UnitTestsView SEARCH d.a == a.a && d.c == a.c && d.b == a.b OPTIONS {waitForSync: true} SORT d.b ASC RETURN d").toArray();

    assertEqual(result.length, expected.length);
      var i = 0;
      result.forEach(function(res) {
        var doc = expected[i++];
        assertEqual(doc.a, res.a);
        assertEqual(doc.b, res.b);
        assertEqual(doc.c, res.c);
      });
    },

    testViewInSubquery : function() {
      var entitiesData = [
        {
          "_key": "person1",
          "_id": "entities/person1",
          "_rev": "_YOr40eu--_",
          "type": "person",
          "id": "person1"
        },
        {
          "_key": "person5",
          "_id": "entities/person5",
          "_rev": "_YOr48rO---",
          "type": "person",
          "id": "person5"
        },
        {
          "_key": "person4",
          "_id": "entities/person4",
          "_rev": "_YOr5IGu--_",
          "type": "person",
          "id": "person4"
        },
        {
          "_key": "person3",
          "_id": "entities/person3",
          "_rev": "_YOr5PBK--_",
          "type": "person",
          "id": "person3"
        },
        {
          "_key": "person2",
          "_id": "entities/person2",
          "_rev": "_YOr5Umq--_",
          "type": "person",
          "id": "person2"
        }
      ];

      var linksData = [
        {
          "_key": "3301",
          "_id": "links/3301",
          "_from": "entities/person1",
          "_to": "entities/person2",
          "_rev": "_YOrbp_S--_",
          "type": "relationship",
          "subType": "married",
          "from": "person1",
          "to": "person2"
        },
        {
          "_key": "3377",
          "_id": "links/3377",
          "_from": "entities/person4",
          "_to": "entities/person5",
          "_rev": "_YOrbxN2--_",
          "type": "relationship",
          "subType": "married",
          "from": "person4",
          "to": "person5"
        },
        {
          "_key": "3346",
          "_id": "links/3346",
          "_from": "entities/person1",
          "_to": "entities/person3",
          "_rev": "_YOrb4kq--_",
          "type": "relationship",
          "subType": "married",
          "from": "person1",
          "to": "person3"
        }
      ];

      // create entities collection
      var entities = db._createDocumentCollection("entities");

      entitiesData.forEach(function(doc) {
        entities.save(doc);
      });

      // create links collection
      var links = db._createEdgeCollection("links");
      linksData.forEach(function(doc) {
        links.save(doc);
      });

      var entitiesView = db._createView("entities_view", "arangosearch",{
        "writebufferSizeMax": 33554432,
        "consolidationPolicy": {
          "type": "bytes_accum",
          "threshold": 0.10000000149011612
        },
        "globallyUniqueId": "hB4A95C21732A/218",
        "id": "218",
        "writebufferActive": 0,
        "consolidationIntervalMsec": 60000,
        "cleanupIntervalStep": 10,
        "links": {
          "entities": {
            "analyzers": [
              "identity"
            ],
            "fields": {},
            "includeAllFields": true,
            "storeValues": "id",
            "trackListPositions": false
          }
        },
        "type": "arangosearch",
        "writebufferIdle": 64
      });

      var linksView = db._createView("links_view", "arangosearch",{
        "writebufferSizeMax": 33554432,
        "consolidationPolicy": {
          "type": "bytes_accum",
          "threshold": 0.10000000149011612
        },
        "globallyUniqueId": "hB4A95C21732A/181",
        "id": "181",
        "writebufferActive": 0,
        "consolidationIntervalMsec": 60000,
        "cleanupIntervalStep": 10,
        "links": {
          "links": {
            "analyzers": [
              "identity"
            ],
            "fields": {},
            "includeAllFields": true,
            "storeValues": "id",
            "trackListPositions": false
          }
        },
        "type": "arangosearch",
        "writebufferIdle": 64
      });

      var expectedResult = [
        { id: "person1", marriedIds: ["person2", "person3"] },
        { id: "person2", marriedIds: ["person1" ] },
        { id: "person3", marriedIds: ["person1" ] },
        { id: "person4", marriedIds: ["person5" ] },
        { id: "person5", marriedIds: ["person4" ] }
      ];

      var queryString = 
        "FOR org IN entities_view SEARCH org.type == 'person' OPTIONS {waitForSync:true} " + 
        "LET marriedIds = ( " +
        " LET entityIds = ( " +
        " FOR l IN links_view SEARCH l.type == 'relationship' AND l.subType == 'married' AND (l.from == org.id OR l.to == org.id) OPTIONS {waitForSync:true} " +
        "    RETURN DISTINCT l.from == org.id ? l.to : l.from" +
        "  ) " +
        "  FOR entityId IN entityIds SORT entityId RETURN entityId " +
        ") " +
        "LIMIT 10 " +
        "SORT org._key " + 
        "RETURN { id: org._key, marriedIds: marriedIds }";

      var result = db._query(queryString).toArray();

      assertEqual(result.length, expectedResult.length);

      var i = 0;
      result.forEach(function(doc) {
        var expectedDoc = expectedResult[i++];
        assertEqual(expectedDoc.org, doc.org);
        assertEqual(expectedDoc.marriedIds, doc.marriedIds);
      });

      entitiesView.drop();
      linksView.drop();
      entities.drop();
      links.drop();
    },

    testAttributeInRangeOpenInterval : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH IN_RANGE(doc.c, 1, 3, false, false) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 4);
      result.forEach(function(res) {
        assertTrue(res.c > 1 && res.c < 3);
      });
    },

    testAttributeInRangeClosedInterval : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH IN_RANGE(doc.c, 1, 3, true, true) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 12);
      result.forEach(function(res) {
        assertTrue(res.c >= 1 && res.c <= 3);
      });
    },
    testAttributeNotInRangeOpenInterval : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH NOT(IN_RANGE(doc.c, 1, 3, false, false)) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 24);
      result.forEach(function(res) {
        assertTrue(res.c === undefined || res.c <= 1 || res.c >= 3);
      });
    },
    testAttributeNotInRangeClosedInterval : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH NOT(IN_RANGE(doc.c, 1, 3, true, true)) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 16);
      result.forEach(function(res) {
        assertTrue(res.c === undefined || res.c < 1 || res.c > 3);
      });
    },
    testAttributeInRange : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c IN 1..3 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 12);
      result.forEach(function(res) {
        assertTrue(res.c >= 1 || res.c <= 3);
      });
    },
    testAttributeNotInRange : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c NOT IN 1..3 OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 16);
      result.forEach(function(res) {
        assertTrue(res.c === undefined || res.c < 1 || res.c > 3);
      });
    },
    testAttributeInArray : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c IN [ 1, 3 ] OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 8);
      result.forEach(function(res) {
        assertTrue(res.c === 1 || res.c === 3);
      });
    },
    testAttributeNotInArray : function () {
      var result = db._query("FOR doc IN UnitTestsView SEARCH doc.c NOT IN [ 1, 3 ] OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 20);
      result.forEach(function(res) {
        assertTrue(res.c === undefined || res.c !== 1 && res.c !== 3);
      });
    },
    testAttributeInExpression : function () {
      var result = db._query("FOR c IN [[[1, 3]]] FOR doc IN UnitTestsView  SEARCH 1 IN FLATTEN(c) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, db.UnitTestsCollection.toArray().length);

    },
    testAttributeNotInExpression: function () {
      var result = db._query("FOR c IN [[[1, 3]]] FOR doc IN UnitTestsView  SEARCH 1 NOT IN FLATTEN(c) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 0);
    },
    testAttributeInExpressionNonDet : function () {
      var result = db._query("FOR c IN [[[1, 3]]] FOR doc IN UnitTestsView  SEARCH 1 IN NOOPT(FLATTEN(c)) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, db.UnitTestsCollection.toArray().length);

    },
    testAttributeNotInExpressionNonDet: function () {
      var result = db._query("FOR c IN [[[1, 3]]] FOR doc IN UnitTestsView  SEARCH 1 NOT IN NOOPT(FLATTEN(c)) OPTIONS { waitForSync : true } RETURN doc").toArray();

      assertEqual(result.length, 0);
    },
    testAnalyzerFunctionPrematureCall : function () {
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH ANALYZER(d.a IN TOKENS('#', 'text_en'), 'text_en') OPTIONS { waitForSync : true } RETURN d").toArray().length,
        0);
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH ANALYZER(d.a NOT IN TOKENS('#', 'text_en'), 'text_en') OPTIONS { waitForSync : true } RETURN d").toArray().length,
        28);
    },
    testBoostFunctionPrematureCall : function () {
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH BOOST(d.a IN TOKENS('#', 'text_en'), 2) OPTIONS { waitForSync : true }  SORT BM25(d) RETURN d").toArray().length,
        0);
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH BOOST(d.a NOT IN TOKENS('#', 'text_en'), 2) OPTIONS { waitForSync : true }  SORT BM25(d) RETURN d").toArray().length,
        28);
    },
    testMinMatchFunctionPrematureCall : function () {
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH MIN_MATCH(d.a IN TOKENS('#', 'text_en'), d.a IN TOKENS('#', 'text_de'), 1) OPTIONS { waitForSync : true } RETURN d").toArray().length,
        0);
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH MIN_MATCH(false, true, true, 2) OPTIONS { waitForSync : true }  RETURN d").toArray().length,
        28);
      assertEqual(
        db._query("FOR d in UnitTestsView SEARCH MIN_MATCH(false, false, false, 0) OPTIONS { waitForSync : true }  RETURN d").toArray().length,
        28);
    },
    testViewWithInterruptedInserts : function() {
      let docsCollectionName = "docs";
      let docsViewName  = "docs_view";
      try { db._drop(docsCollectionName); } catch(e) {}
      try { db._dropView(docsViewName); } catch(e) {}
      let docsCollection = db._create(docsCollectionName);
      let docsView = db._createView(docsViewName, "arangosearch", {
        "links": {
            "docs": {
              "analyzers": ["identity"],
              "fields": {},
              "includeAllFields": true,
              "storeValues": "id",
              "trackListPositions": false
            }
          } ,
        consolidationIntervalMsec:0,
        cleanupIntervalStep:0
      });
      let docs = [];
      for (let i = 0; i < 10; i++) {
        let docId = "TestDoc" + i;
        docs.push({ _id: "docs/" + docId, _key: docId, "indexField": i }); 
      }
      docsCollection.save(docs);

      // test singleOperationTransaction
      try {
        docsCollection.save(docs[5]);
        fail();
      } catch(e) {
        assertEqual(ERRORS.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code, e.errorNum);
      }
      assertEqual(docs.length, db._query("FOR u IN " + docsCollectionName + 
                                         " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length, db._query("FOR u IN " + docsViewName + 
                                        " OPTIONS { waitForSync : true }  COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);

      // testMultipleOparationTransaction (no index revert as PK will be violated)
      let docsNew = [];
      for (let i = 11; i < 20; i++) {
        let docId = "TestDoc" + i;
        docsNew.push({ _id: "docs/" + docId, _key: docId, "indexField": i }); 
      }
      docsNew.push(docs[5]); // this one will cause PK violation 
      docsCollection.save(docsNew);
      assertEqual(docs.length + docsNew.length - 1,
                 db._query("FOR u IN " + docsCollectionName + 
                           " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length + docsNew.length - 1,
                 db._query("FOR u IN " + docsViewName + 
                           " OPTIONS { waitForSync : true }  COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);

      // add another index (to make it fail after arangosearch insert passed)
      docsCollection.ensureIndex({type: "hash", unique: true, fields:["indexField"]});
      
      // single operation insert (will fail on unique index)
      try {
        docsCollection.save({ _id: "docs/fail", _key: "fail", "indexField": 0 });
        fail();
      } catch(e) {
        assertEqual(ERRORS.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED.code, e.errorNum);
      }
      assertEqual(docs.length + docsNew.length - 1,
                 db._query("FOR u IN " + docsCollectionName + 
                           " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length + docsNew.length - 1,
                 db._query("FOR u IN " + docsViewName + 
                           " OPTIONS { waitForSync : true } COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);

      // testMultipleOparationTransaction  (arangosearch index revert will be needed)
      let docsNew2 = [];
      for (let i = 21; i < 30; i++) {
        let docId = "TestDoc" + i;
        docsNew2.push({ _id: "docs/" + docId, _key: docId, "indexField": i }); 
      }
      docsNew2.push({ _id: "docs/fail2", _key: "fail2", "indexField": 0 });// this one will cause hash unique violation 
      docsCollection.save(docsNew2);
      assertEqual(docs.length + docsNew.length  + docsNew2.length - 2,
                 db._query("FOR u IN " + docsCollectionName + 
                           " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length + docsNew.length  + docsNew2.length - 2,
                 db._query("FOR u IN " + docsViewName + 
                           " OPTIONS { waitForSync : true } COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);

      db._drop(docsCollectionName);
      db._dropView(docsViewName);
    },

    testViewWithInterruptedUpdates : function() {
      let docsCollectionName = "docs";
      let docsViewName  = "docs_view";
      try { db._drop(docsCollectionName); } catch(e) {}
      try { db._dropView(docsViewName); } catch(e) {}
      let docsCollection = db._create(docsCollectionName);
      let docsView = db._createView(docsViewName, "arangosearch", {
        "links": {
            "docs": {
              "analyzers": ["identity"],
              "fields": {},
              "includeAllFields": true,
              "storeValues": "id",
              "trackListPositions": false
            }
          } ,
        consolidationIntervalMsec:0,
        cleanupIntervalStep:0
      });

      let docs = [];
      for (let i = 0; i < 10; i++) {
        let docId = "TestDoc" + i;
        docs.push({ _id: "docs/" + docId, _key: docId, "indexField": i }); 
      }
      docsCollection.save(docs);
      // sanity check. Should be in sync
      assertEqual(docs.length, db._query("FOR u IN " + docsCollectionName + 
                                         " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length, db._query("FOR u IN " + docsViewName + 
                                        " OPTIONS { waitForSync : true }  COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);


      // add another index (to make it fail after arangosearch update passed)
      docsCollection.ensureIndex({type: "hash", unique: true, fields:["indexField"]});

      let docsUpdateIds = [];
      let docsUpdateData = [];
      docsUpdateIds.push(docs[0]._id);
      docsUpdateData.push({"indexField": 999}); // valid
      docsUpdateIds.push(docs[1]._id);
      docsUpdateData.push({"indexField": docs[2].indexField}); // will be conflict
      docsCollection.update(docsUpdateIds, docsUpdateData);

      // documents should stay consistent
      let collectionDocs =  db._query("FOR d IN " + docsCollectionName + " SORT d._id ASC RETURN d").toArray();
      let viewDocs = db._query("FOR  d IN " + docsViewName + " OPTIONS { waitForSync : true }  SORT d._id ASC RETURN d").toArray();
      assertEqual(collectionDocs.length, viewDocs.length);
      for(let i = 0; i < viewDocs.length; i++) {
        assertEqual(viewDocs[i]._id, collectionDocs[i]._id);
      }
      db._drop(docsCollectionName);
      db._dropView(docsViewName);
    },

    testViewWithInterruptedRemoves : function() {
      if (isServer && internal.debugCanUseFailAt()) {
        internal.debugClearFailAt();
      }
      let docsCollectionName = "docs";
      let docsViewName  = "docs_view";
      try { db._drop(docsCollectionName); } catch(e) {}
      try { db._dropView(docsViewName); } catch(e) {}
      let docsCollection = db._create(docsCollectionName);
      let docsView = db._createView(docsViewName, "arangosearch", {
        "links": {
            "docs": {
              "analyzers": ["identity"],
              "fields": {},
              "includeAllFields": true,
              "storeValues": "id",
              "trackListPositions": false
            }
          } ,
        consolidationIntervalMsec:0,
        cleanupIntervalStep:0
      });

      let docs = [];
      for (let i = 0; i < 10; i++) {
        let docId = "TestDoc" + i;
        docs.push({ _id: "docs/" + docId, _key: docId, "indexField": i }); 
      }
      docsCollection.save(docs);
      // sanity check. Should be in sync
      assertEqual(docs.length, db._query("FOR u IN " + docsCollectionName + 
                                         " COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);
      assertEqual(docs.length, db._query("FOR u IN " + docsViewName + 
                                        " OPTIONS { waitForSync : true }  COLLECT WITH COUNT INTO length RETURN length").toArray()[0]);


      // add another index (to make it fail after arangosearch remove passed)
      docsCollection.ensureIndex({type: "hash", unique: true, fields:["indexField"]});

      // check removes are sane. Both server and client
      { 
        let docsRemoveIds = [];
        docsRemoveIds.push(docs[0]._id);
        docsRemoveIds.push(docs[1]._id);
        docsCollection.remove(docsRemoveIds); 
        // documents should stay consistent
        let collectionDocs =  db._query("FOR d IN " + docsCollectionName + " SORT d._id ASC RETURN d").toArray();
        let viewDocs = db._query("FOR  d IN " + docsViewName + " OPTIONS { waitForSync : true }  SORT d._id ASC RETURN d").toArray();
        assertEqual(collectionDocs.length, viewDocs.length);
        for(let i = 0; i < viewDocs.length; i++) {
          assertEqual(viewDocs[i]._id, collectionDocs[i]._id);
        }
      }

      // on server make full check with failures
      if (isServer && internal.debugCanUseFailAt()) { 
        let docsRemoveIds = [];
        docsRemoveIds.push(docs[2]._id);
        docsRemoveIds.push(docs[3]._id);
        internal.debugSetFailAt('BreakHashIndexRemove'); 
        docsCollection.remove(docsRemoveIds); 
        internal.debugRemoveFailAt('BreakHashIndexRemove');
        // documents should stay consistent
        let collectionDocs =  db._query("FOR d IN " + docsCollectionName + " SORT d._id ASC RETURN d").toArray();
        let viewDocs = db._query("FOR  d IN " + docsViewName + " OPTIONS { waitForSync : true }  SORT d._id ASC RETURN d").toArray();
        assertEqual(collectionDocs.length, docs.length - 2); // removes should not pass if failpoint worked!
        assertEqual(collectionDocs.length, viewDocs.length);
        for(let i = 0; i < viewDocs.length; i++) {
          assertEqual(viewDocs[i]._id, collectionDocs[i]._id);
        }
      }
     
      db._drop(docsCollectionName);
      db._dropView(docsViewName);
    },

  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(iResearchAqlTestSuite);

return jsunity.done();
