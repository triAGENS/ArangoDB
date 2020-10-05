/*global assertEqual, assertFalse, assertNull, assertTrue, AQL_EXECUTE */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for query language, bind parameters
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Copyright 2020, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const cn = "UnitTestsAhuacatlUpsert";
const db = require("@arangodb").db;
const jsunity = require("jsunity");
const isCluster = require("internal").isCluster();
const debugLogging = false;



class UpsertTestCase {
  constructor(flags) {
    this._flags = flags;
    this._differentValues = 10;
    // We will have 1 full batch of 1000 rows (500 data, 500 shadow)
    // and a second batch with fewer docs.
    this._subqueryRuns = 510;
    // Flag parsing, requires _flags to be set
    this._activateOptimizerRules = this._isBitSet(0);
    this._triggerInsert = this._isBitSet(1);
    this._largeLoop = this._isBitSet(2);
    this._uniqueIndex = this._isBitSet(3);
    this._inSubqueryLoop = this._isBitSet(4);
    // TODO: Reactivate this, right now those tests
    // are red as they can read your own write.
    this._updateOnly = false;
    // this._updateOnly = this._isBitSet(5);
    this._index = this._isBitSet(6);


    // Cluster only
    // Make sure those are the highest bits
    // as the single server will simply not set them
    this._multipleShards = this._isBitSet(7);
    this._shardKey = this._isBitSet(8);
    this._satellite = this._isBitSet(9);
  }

  isValidCombination() {
    if (isCluster && this._inSubqueryLoop) {
      // This does not work in cluster due to timeout.
      return false;
    }
    if (this._inSubqueryLoop && this._largeLoop) {
      // This will generate 5mio operations, that is too much to test
      return false;
    }
    if (this._updateOnly && this._triggerInsert) {
      // A variant that inserts documents, cannot be translated into an update only statement
      return false;
    }
    if (this._uniqueIndex && this._index) {
      // We can either have unique or non-unique or no index.
      return false;
    }
    if (this._satellite && (this._multipleShards || this._shardKey)) {
      // Shardkey or more then one shard are irrelavant for satellites
      return false;
    }
    if (this._uniqueIndex && this._multipleShards && !this._shardKey) {
      // We can only have a uniqueIndex on the shardKey, if we have
      // more then 1 shard.
      return false;
    }
    return true;
  }

  /*
   * Public Methods
   */
  name() {
    const nameFlags = ["test", `${this._docsPerLoop()}`];
    if (this._inSubqueryLoop) {
      nameFlags.push(`${this._subqueryRuns}_subqueries`);
    }
    if (this._triggerInsert) {
      nameFlags.push("Insert");
    } else if (this._updateOnly) {
      nameFlags.push("PlainUpdate");
    } else {
      nameFlags.push("UpsertUpdate");
    }

    if (this._uniqueIndex) {
      nameFlags.push("UniqueIndex");
    }
    if (this._index) {
      nameFlags.push("Indexed");
    }
    if (this._satellite) {
      nameFlags.push("Satellite");
    }
    if (this._multipleShards) {
      nameFlags.push("5_shards");
    }
    if (this._shardKey) {
      nameFlags.push("ShardByValue");
    }
    if (!this._activateOptimizerRules) {
      nameFlags.push("NoOpt");
    }
    return nameFlags.join("_");
  }



  runTest() {
    this._prepareData();

    const query = this._generateQuery();
    // Query will have side effects
    db._query(query, {},  this._options());
    this._validateResult();
    

  }

  /*
   * Private Methods
   */


  _prepareData() {
    {
      const collectionOptions = {};
      collectionOptions.numberOfShards = this._multipleShards ? 5 : 1;
      if (this._shardKey) {
        collectionOptions.shardKeys = ["value"];
      }
      if (this._satellite) {
        collectionOptions.replicationFactor = "satellite";
      }
      db._create(cn, collectionOptions);
    }
    
    if (this._uniqueIndex) {
      this._col().ensureUniqueConstraint("value");
    }
    if (!this._triggerInsert) {
      const docs = [];
      for (let value = 0; value < this._differentValues; ++value) {
        docs.push({
          value,
          count: 1
        });
      }
      this._col().save(docs);
      assertEqual(this._col().count(), this._differentValues, this._col().toArray());
    } else {
      assertEqual(this._col().count(), 0);
    }
  }

  _validateResult() {
    const expected = this._generateExpectedResult();
    if (debugLogging) {
      for (const [value, count] of expected.entries()) {
        require("internal").print(`${value} => ${count}`);
      }
    }
    const allDocs = this._col().toArray();
    if (debugLogging) {
      require("internal").print(allDocs);
    }
    assertEqual(allDocs.length, 10, "More documents than expected, INSERT triggered to often");
    for (const doc of allDocs) {
      if (debugLogging) {
        require("internal").print(doc);
      }

      assertTrue(expected.has(doc.value));
      assertEqual(expected.get(doc.value), doc.count);
    }

  }

  _generateQuery() {
    let modificationPart;
    if (this._updateOnly) {
      // This performs the same operations as upsert does
      // However the guarantee here is that we do not read our own writes.
      modificationPart = `
        FOR base IN 0..${this._docsPerLoop() - 1}
        LET value = base % ${this._differentValues}
        LET search = FIRST((
          FOR tmp IN ${cn}
            FILTER tmp.value == value
            RETURN tmp
        ))
        UPDATE search WITH {count: search.count + 1} IN ${cn}
      `;
    } else {
      modificationPart = `
      FOR base IN 0..${this._docsPerLoop() - 1}
      LET value = base % ${this._differentValues}
        UPSERT { value } INSERT {value, count: 1} UPDATE {count: OLD.count + 1} IN ${cn}
      `;
    }
    if (this._inSubqueryLoop) {
      return `
        FOR k IN 1..${this._subqueryRuns}
          LET sub = (
            ${modificationPart}
          )
          RETURN k
      `;
    }
    return modificationPart;
  }

  _options() {
    const options = {};
    if (this._activateOptimizerRules) {
      options.optimizer = {rules: ["+all"]};
    } else {
      options.optimizer = {rules: ["-all"]};
    }
    if (true && debugLogging) {
      options.profile = 4;
    }
    return options;
  }

  _generateExpectedResult() {
    const expected = new Map();
    if (this._updateOnly) {
      // This does not allow to read your own write.
      // Hence we can ONLY update from 1 => 2
      // And we will do so in all queries.
      for (let value = 0; value < this._differentValues; ++value) {
        expected.set(value, 2);
      }
      return expected;
    }
    const initValue = this._triggerInsert ? 0 : 1;
    for (let value = 0; value < this._differentValues; ++value) {
      expected.set(value, initValue);
    }
    for (let j = 0; j < (this._inSubqueryLoop ? this._subqueryRuns : 1); ++j) {
      for (let i = 0; i < this._docsPerLoop(); ++i) {
        const value = i % this._differentValues;
        expected.set(value, expected.get(value) + 1);
      }
    }
    return expected;
  }

  _col() {
    return db._collection(cn);
  }

  _isBitSet(bit) {
    return (this._flags >> bit) & 1 === 1;
  }

  _docsPerLoop() {
    if (this._largeLoop) {
      // This is above 1000 which is current Batchsize. adjust if necessary.
      return 1010;
    }
    return 20;
  }


}

function aqlUpsertCombinationSuite () {

  const clear = () => {
    db._drop(cn);
  };

  const testSuite = {
    // Every test will create the Collection
    // As the cluster test require special sharding
    setUp : clear,

////////////////////////////////////////////////////////////////////////////////
/// @brief tear down
////////////////////////////////////////////////////////////////////////////////

    tearDown : clear
  };
  
  let testFlags = 7;
  if (isCluster) {
    // Sharding Mechanism
    testFlags += 3;
  }
  const numTests = Math.pow(2, testFlags);
  for (let i = 0; i < numTests; ++i) {
    const tCase = new UpsertTestCase(i);
    if (tCase.isValidCombination()) {
      testSuite[tCase.name()] = () => {
        tCase.runTest();
      };
    }

  }  
  return testSuite;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(aqlUpsertCombinationSuite);

return jsunity.done();