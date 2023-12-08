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
////////////////////////////////////////////////////////////////////////////////

let arangodb = require("@arangodb");
let analyzers = require("@arangodb/analyzers");
let db = arangodb.db;
let internal = require("internal");
let jsunity = require("jsunity");
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;

function ArangoSearchWildcardAnalyzer(hasPos) {
  const dbName = "ArangoSearchWildcardAnalyzer" + hasPos;

  let cleanup = function() {
    internal.debugClearFailAt();
    db._useDatabase("_system");
    try { db._dropDatabase(dbName); } catch (err) {}
  };

  let query = function(pattern, needsPrefix, needsMatcher) {
    internal.debugClearFailAt();
    if (needsPrefix) {
      internal.debugSetFailAt("wildcard::Filter::needsPrefix");
    } else {
      internal.debugSetFailAt("wildcard::Filter::dissallowPrefix");
    }
    if (needsMatcher) {
      internal.debugSetFailAt("wildcard::Filter::needsMatcher");
    } else {
      internal.debugSetFailAt("wildcard::Filter::dissallowMatcher");
    }
    let r = db._query("FOR d in v SEARCH ANALYZER(d.s LIKE '" + pattern + "', 'w') RETURN d.s").toArray().sort();
    return r;
  };

  return {
    setUpAll : function () { 
      cleanup();

      db._createDatabase(dbName);
      db._useDatabase(dbName);
      if (hasPos) {
        analyzers.save("w", "wildcard", { ngramSize: 4 }, ["frequency", "position"]);
      } else {
        analyzers.save("w", "wildcard", { ngramSize: 4 }, []);
      }
      let c = db._create("c");
      c.insert({s: ""});
      c.insert({s: "abcdef"});
      c.insert({s:  "bcde"});
      c.insert({s:   "c"});
      c.insert({s: "qwerty"});
      c.insert({s:  "wert"});
      c.insert({s:   "e"});
      c.insert({s:   "a"});
      c.insert({s:   "f"});
      c.insert({s: "abcdef qwerty"});
      c.insert({s: "qwerty abcdef"});
      db._createView("v", "arangosearch",
          { links: { c: { fields: {s: {}}, analyzers: ["w"] } } });
    },

    tearDownAll : function () { 
      cleanup();
    },

    testExactEmpty: function() {
      let res = query("", false, false);
      assertEqual(res, [""]);
    },

    testExactShort: function() {
      let res = query("c", false, false);
      assertEqual(res, ["c"]);
    },

    testExactMid: function() {
      let res = query("bcde", false, !hasPos);
      assertEqual(res, ["bcde"]);
    },

    testExactLong: function() {
      let res = query("abcdef", false, !hasPos);
      assertEqual(res, ["abcdef"]);
    },

    testAnyShort: function() {
      let res = query("_", false, true);
      assertEqual(res, ["a", "c", "e", "f"]);
    },

    testAnyMid: function() {
      let res = query("bcd_", false, true);
      assertEqual(res, ["bcde"]);
    },

    testAnyLong: function() {
      let res = query("a_cd_f", false, true);
      assertEqual(res, ["abcdef"]);
    },

    testPrefixShort: function() {
      let res = query("a%", true, false);
      assertEqual(res, ["a", "abcdef", "abcdef qwerty"]);
    },

    testPrefixMed: function() {
      let res = query("abcd%", false, !hasPos);
      assertEqual(res, ["abcdef", "abcdef qwerty"]);
    },

    testPrefixLong: function() {
      let res = query("abcdef%", false, !hasPos);
      assertEqual(res, ["abcdef", "abcdef qwerty"]);
    },

    testSuffixShort: function() {
      let res = query("%f", false, false);
      assertEqual(res, ["abcdef", "f", "qwerty abcdef"]);
    },

    testSuffixMed: function() {
      let res = query("%cdef", false, !hasPos);
      assertEqual(res, ["abcdef", "qwerty abcdef"]);
    },

    testSuffixLong: function() {
      let res = query("%abcdef", false, !hasPos);
      assertEqual(res, ["abcdef", "qwerty abcdef"]);
    },

    testAllShort: function() {
      let res = query("%c%", true, false);
      assertEqual(res, ["abcdef", "abcdef qwerty", "bcde", "c", "qwerty abcdef"]);
    },

    testAllMed: function() {
      let res = query("%bcde%", false, !hasPos);
      assertEqual(res, ["abcdef", "abcdef qwerty", "bcde", "qwerty abcdef"]);
    },

    testAllLong: function() {
      let res = query("%abcdef%", false, !hasPos);
      assertEqual(res, ["abcdef", "abcdef qwerty", "qwerty abcdef"]);
    },

    testAll: function() {
      let res = query("%", false, false);
      assertEqual(res.length, 11);
      res = query("%%", false, false);
      assertEqual(res.length, 11);
      res = query("%%%", false, true);
      assertEqual(res.length, 11);
    },
  };
}

function ArangoSearchWildcardAnalyzerNoPos() {
  let suite = {};
  deriveTestSuite(
    ArangoSearchWildcardAnalyzer(false),
    suite,
    "_no_pos"
  );
  return suite;
}

function ArangoSearchWildcardAnalyzerHasPos() {
  let suite = {};
  deriveTestSuite(
    ArangoSearchWildcardAnalyzer(true),
    suite,
    "_has_pos"
  );
  return suite;
}

jsunity.run(ArangoSearchWildcardAnalyzerNoPos);
jsunity.run(ArangoSearchWildcardAnalyzerHasPos);

return jsunity.done();
