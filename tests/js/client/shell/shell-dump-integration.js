/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, assertTrue, assertFalse, assertEqual, assertNotEqual, arango */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2018 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
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
// / @author Jan Steemann
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const internal = require('internal');
const arangodb = require('@arangodb');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const db = arangodb.db;
const isCluster = require("internal").isCluster();
const dbs = ["_system", "maçã", "😀", "ﻚﻠﺑ ﻞﻄﻴﻓ", "testName"];
const extendedName = "Десятую Международную Конференцию по 💩🍺🌧t⛈c🌩_⚡🔥💥🌨";
const { executeExternalAndWaitWithSanitizer } = require('@arangodb/test-helper');

const validatorJson = {
  "message": "",
  "level": "new",
  "type": "json",
  "rule": {
    "additionalProperties": true,
    "properties": {
      "value1": {
        "type": "integer"
      },
      "value2": {
        "type": "string"
      },
      "name": {
        "type": "string"
      }
    },
    "required": [
      "value1",
      "value2"
    ],
    "type": "object"
  }
};

function checkDumpJsonFile(dbName, path, id) {
  let data = JSON.parse(fs.readFileSync(fs.join(path, "dump.json")).toString());
  assertEqual(dbName, data.properties.name);
  assertEqual(id, data.properties.id);
}

function dumpIntegrationSuite() {
  'use strict';
  const cn = 'UnitTestsDump';
  const arangodump = pu.ARANGODUMP_BIN;

  assertTrue(fs.isFile(arangodump), "arangodump not found!");

  let addConnectionArgs = function (args) {
    let endpoint = arango.getEndpoint().replace(/\+vpp/, '').replace(/^http:/, 'tcp:').replace(/^https:/, 'ssl:').replace(/^vst:/, 'tcp:').replace(/^h2:/, 'tcp:');
    args.push('--server.endpoint');
    args.push(endpoint);
    if (args.indexOf("--all-databases") === -1) {
      args.push('--server.database');
      args.push(arango.getDatabaseName());
    }
    args.push('--server.username');
    args.push(arango.connectedUser());
  };

  let runDump = function (path, args, expectRc) {
    args.push('--output-directory');
    args.push(path);
    addConnectionArgs(args);

    let actualRc = executeExternalAndWaitWithSanitizer(arangodump, args, 'shell-dump-integration');
    assertTrue(actualRc.hasOwnProperty("exit"));
    assertEqual(expectRc, actualRc.exit);
    return fs.listTree(path);
  };

  let checkEncryption = function (tree, path, expected) {
    assertNotEqual(-1, tree.indexOf("ENCRYPTION"));
    let data = fs.readFileSync(fs.join(path, "ENCRYPTION")).toString();
    assertEqual(expected, data);
  };

  let structureFile = function (path, cn, escapedName = undefined) {
    if (escapedName === undefined) {
      escapedName = cn;
    }
    const prefix = escapedName + "_" + require("@arangodb/crypto").md5(cn);
    let structure = prefix + ".structure.json";
    if (!fs.isFile(fs.join(path, structure))) {
      // seems necessary in cluster
      structure = escapedName + ".structure.json";
    }
    return structure;
  };

  let checkStructureFile = function (tree, path, readable, cn, subdir = "", escapedName = undefined) {
    let structurePath = path;
    if (subdir !== "") {
      structurePath = fs.join(path, subdir);
    }
    let structure = structureFile(structurePath, cn, escapedName);
    if (subdir !== "") {
      structure = fs.join(subdir, structure);
    }
    assertTrue(fs.isFile(fs.join(path, structure)), structure);
    assertNotEqual(-1, tree.indexOf(structure));

    if (readable) {
      let data = JSON.parse(fs.readFileSync(fs.join(path, structure)).toString());
      assertEqual(cn, data.parameters.name);
      if (cn.endsWith("WithSchema")) {
        assertTrue(data.parameters.hasOwnProperty("schema"));
        const schema = data.parameters.schema;
        assertEqual(schema, validatorJson);
      }
    } else {
      try {
        // cannot read encrypted file
        JSON.parse(fs.readFileSync(fs.join(path, structure)));
        fail();
      } catch (err) {
        // error is expected here
        assertTrue(err instanceof SyntaxError, err);
      }
    }
  };

  let checkCollections = function (tree, path, subdir = "") {
    db._collections().forEach((collectionObj) => {
      const collectionName = collectionObj.name();
      if (!collectionName.startsWith("_")) {
        checkStructureFile(tree, path, true, collectionName, subdir);
      }
    });
  };

  let checkDataFileInternal = function (tree, path, split, compressed, envelopes, readable, cn, escapedName, checkFn) {
    const prefix = escapedName + "_" + require("@arangodb/crypto").md5(cn);

    let getFiles = function (prefix, split, compressed) {
      let files = tree.filter((f) => f.startsWith(prefix + "."));

      if (split) {
        files = files.filter((f) => f.match(/\.\d+\.data\.json/));
      } else {
        files = files.filter((f) => f.match(/\.data\.json/));
      }
      if (compressed) {
        files = files.filter((f) => f.endsWith("data.json.gz"));
      } else {
        files = files.filter((f) => f.endsWith("data.json"));
      }
      files.sort();
      return files;
    };

    let files = getFiles(prefix, split, compressed);
    let altFiles = getFiles(prefix, split, !compressed);
    assertNotEqual(0, files.length, files);
    assertEqual(0, altFiles.length, altFiles);

    let data = [];
    if (compressed) {
      assertTrue(readable);
      files.forEach((f) => {
        data = data.concat(fs.readGzip(fs.join(path, f)).toString().trim().split('\n'));
      });
      checkFn(data, envelopes);
    } else {
      if (readable) {
        files.forEach((f) => {
          data = data.concat(fs.readFileSync(fs.join(path, f)).toString().trim().split('\n'));
        });
        checkFn(data, envelopes);
      } else {
        files.forEach((f) => {
          try {
            // cannot read encrypted files
            JSON.parse(fs.readFileSync(fs.join(path, f)));
            fail();
          } catch (err) {
            // error is expected here
            assertTrue(err instanceof SyntaxError, err);
          }
        });
      }
    }
  };

  let checkDataFileForCollectionWithComputedValues = function (tree, path, compressed, envelopes, readable, cn) {
    return checkDataFileInternal(tree, path, /*split*/ false, compressed, envelopes, readable, cn, cn, function (data, envelopes) {
      assertEqual(1000, data.length);
      data.forEach(function (line) {
        line = JSON.parse(line);
        if (envelopes) {
          assertEqual(2300, line.type);
          assertTrue(line.hasOwnProperty('data'));
          assertTrue(line.data.hasOwnProperty('_key'));
          assertTrue(line.data.hasOwnProperty('_rev'));
          assertTrue(line.data.hasOwnProperty('value1'));
          assertTrue(line.data.hasOwnProperty('value2'));
          assertTrue(line.data.hasOwnProperty('value3'));
          assertTrue(line.data.hasOwnProperty('value4'));
          assertEqual(line.value3, line.value1 + "+" + line.value2);
          assertEqual(line.value4, line.value2 + " " + line.value1);
        } else {
          assertFalse(line.hasOwnProperty('type'));
          assertFalse(line.hasOwnProperty('data'));
          assertTrue(line.hasOwnProperty('_key'));
          assertTrue(line.hasOwnProperty('_rev'));
          assertTrue(line.hasOwnProperty('value1'));
          assertTrue(line.hasOwnProperty('value2'));
          assertTrue(line.hasOwnProperty('value3'));
          assertTrue(line.hasOwnProperty('value4'));
          assertEqual(line.value3, line.value1 + "+" + line.value2);
          assertEqual(line.value4, line.value2 + " " + line.value1);
        }
      });
    });
  };
  
  let checkDataFile = function (tree, path, split, compressed, envelopes, readable, cn, escapedName = undefined) {
    if (escapedName === undefined) {
      escapedName = cn;
    }
    return checkDataFileInternal(tree, path, /*split*/ false, compressed, envelopes, readable, cn, escapedName, function (data, envelopes) {
      if (cn.startsWith('_')) {
        // system collection
        assertTrue(data.length > 0);
      } else {
        assertEqual(1000, data.length);
      }
      data.forEach(function (line) {
        if (line.trim() === '') {
          return;
        }
        line = JSON.parse(line);
        if (envelopes) {
          assertEqual(2300, line.type);
          assertTrue(line.hasOwnProperty('data'));
          assertTrue(line.data.hasOwnProperty('_key'));
          assertTrue(line.data.hasOwnProperty('_rev'));
        } else {
          assertFalse(line.hasOwnProperty('type'));
          assertFalse(line.hasOwnProperty('data'));
          assertTrue(line.hasOwnProperty('_key'));
          assertTrue(line.hasOwnProperty('_rev'));
        }
      });
    });
  };

  return {
    setUpAll: function () {
      dbs.forEach((name) => {
        if (name !== "_system") {
          db._useDatabase("_system");
          db._createDatabase(name);
        }
        db._useDatabase(name);
        db._drop(cn);

        let c = db._create(cn, {numberOfShards: 3});
        let docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({_key: "test" + i});
        }
        c.insert(docs);

        db._drop(cn + "Other");
        c = db._create(cn + "Other", {numberOfShards: 3});
        c.insert(docs);

        db._drop(cn + "Padded");
        c = db._create(cn + "Padded", {keyOptions: {type: "padded"}, numberOfShards: 3});
        docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({});
        }
        c.insert(docs);

        db._drop(cn + "AutoIncrement");
        let numShards = 3;
        if (isCluster) {
          numShards = 1;
        }
        c = db._create(cn + "AutoIncrement", {keyOptions: {type: "autoincrement"}, numberOfShards: numShards});
        docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({});
        }
        c.insert(docs);
        c = db._create(cn + "ComputedValues", {
          computedValues: [{
            name: "value3",
            expression: "RETURN CONCAT(@doc.value1, '+', @doc.value2)",
            overwrite: false
          }, {
            name: "value4",
            expression: "RETURN CONCAT(@doc.value2, ' ', @doc.value1)",
            overwrite: true
          }]
        });
        docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({value1: "test" + i, value2: "abc", value4: false});
        }
        c.insert(docs);
        c = db._create(cn + "WithSchema", {schema: validatorJson, numberOfShards: numShards});
        docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({value1: i, value2: "abc"});
        }
        c.insert(docs);
      });
    },

    tearDownAll: function () {
      db._useDatabase("_system");
      dbs.forEach((name) => {
        if (name === "_system") {
          db._drop(cn);
          db._drop(cn + "Other");
          db._drop(cn + "Padded");
          db._drop(cn + "AutoIncrement");
          db._drop(cn + "ComputedValues");
          db._drop(cn + "WithSchema");
        } else {
          db._dropDatabase(name);
        }
      });
    },
    
    setUp: function () {
      db._useDatabase("_system");
    },
    
    tearDown: function () {
      db._useDatabase("_system");
    },
    
    testNonExperimentalDump: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'false'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testExperimentalDumpSingle: function () {
      if (isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, false, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testExperimentalDumpCluster: function () {
      if (!isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testNonExperimentalDumpSplitFiles: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'false', '--split-files', 'true'];
      let tree = runDump(path, args, 1 /*exit code*/);
      assertEqual([""], tree);
    },
    
    testExperimentalDumpSplitFilesSingle: function () {
      if (isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'true', '--split-files', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      // experimental dump and thus splitting are not supported in single server
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testExperimentalDumpSplitFilesCluster: function () {
      if (!isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'true', '--split-files', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ true, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testExperimentalDumpIncludeSystemCollections: function () {
      if (!isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--compress-output', 'false', '--use-experimental-dump', 'true', '--include-system-collections', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkStructureFile(tree, path, true, "_apps");
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, "_apps");
      fs.removeDirectoryRecursive(path, true);
    },
    
    testExperimentalDumpUsersCollection: function () {
      if (!isCluster) {
        return;
      }
      let path = fs.getTempFile();
      let args = ['--collection', cn, '--compress-output', 'false', '--use-experimental-dump', 'true', '--collection', '_users'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkStructureFile(tree, path, true, "_users");
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, "_users");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpForCollectionWithSchema: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn + "WithSchema", '--compress-output', 'false'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn + "WithSchema");
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn + "WithSchema");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpForCollectionWithComputedValuesUncompressed: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn + "ComputedValues", '--compress-output', 'false'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn + "ComputedValues");
      checkDataFileForCollectionWithComputedValues(tree, path, false, false, true, cn + "ComputedValues");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpForCollectionWithComputedValuesCompressed: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn + "ComputedValues", '--compress-output', 'true'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn + "ComputedValues");
      checkDataFileForCollectionWithComputedValues(tree, path, true, false, true, cn + "ComputedValues");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOnlyOneShard: function () {
      if (!isCluster) {
        return;
      }

      let path = fs.getTempFile();

      let c = db._collection(cn);
      let shardCounts = c.count(true);
      let shards = Object.keys(shardCounts);

      assertEqual(3, shards.length);
      let args = ['--collection', cn, '--dump-data', 'true', '--compress-output', 'false', '--shard', shards[0]];
      let tree = runDump(path, args, 0);

      const prefix = cn + "_" + require("@arangodb/crypto").md5(cn);
      let file = fs.join(path, prefix + '.data.json');
      let data = fs.readFileSync(file).toString();
      assertEqual(shardCounts[shards[0]] + 1, data.split('\n').length);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOnlyTwoShards: function () {
      if (!isCluster) {
        return;
      }

      let path = fs.getTempFile();

      let c = db._collection(cn);
      let shardCounts = c.count(true);
      let shards = Object.keys(shardCounts);

      assertEqual(3, shards.length);
      let args = ['--collection', cn, '--dump-data', 'true', '--compress-output', 'false', '--shard', shards[0], '--shard', shards[1]];
      let tree = runDump(path, args, 0);

      const prefix = cn + "_" + require("@arangodb/crypto").md5(cn);
      let file = fs.join(path, prefix + '.data.json');
      let data = fs.readFileSync(file).toString();
      assertEqual(shardCounts[shards[0]] + shardCounts[shards[1]] + 1, data.split('\n').length);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpAutoIncrementKeyGenerator: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn + 'AutoIncrement', '--dump-data', 'false'];
      let tree = runDump(path, args, 0);
      checkStructureFile(tree, path, true, cn + 'AutoIncrement');
      let structure = structureFile(path, cn + 'AutoIncrement');
      let data = JSON.parse(fs.readFileSync(fs.join(path, structure)).toString());
      assertEqual("autoincrement", data.parameters.keyOptions.type);
      let c = db._collection(cn + 'AutoIncrement');
      assertEqual(1000, c.count());
      let p = c.properties();
      let lastValue = p.keyOptions.lastValue;
      if (!isCluster) {
        assertTrue(lastValue > 0, lastValue);
      }
      assertEqual(lastValue, data.parameters.keyOptions.lastValue);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpPaddedKeyGenerator: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn + 'Padded', '--dump-data', 'false'];
      let tree = runDump(path, args, 0);
      checkStructureFile(tree, path, true, cn + 'Padded');
      let structure = structureFile(path, cn + 'Padded');
      let data = JSON.parse(fs.readFileSync(fs.join(path, structure)).toString());
      assertEqual("padded", data.parameters.keyOptions.type);
      let c = db._collection(cn + 'Padded');
      assertEqual(1000, c.count());
      let p = c.properties();
      let lastValue = p.keyOptions.lastValue;
      assertTrue(lastValue > 0, lastValue);
      assertEqual(lastValue, data.parameters.keyOptions.lastValue);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpSingleDatabase: function () {
      dbs.forEach((name) => {
        let path = fs.getTempFile();
        db._useDatabase(name);
        let args = ['--overwrite', 'true'];
        let tree = runDump(path, args, 0);
        checkDumpJsonFile(name, path, db._id());
        checkCollections(tree, path);
        fs.removeDirectoryRecursive(path, true);
      });
    },

    testDumpAllDatabases: function () {
      let path = fs.getTempFile();
      let args = ['--all-databases', 'true'];
      let tree = runDump(path, args, 0);
      db._useDatabase("maçã");
      assertEqual(-1, tree.indexOf("maçã"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("maçã", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      db._useDatabase("_system");
      assertNotEqual(-1, tree.indexOf("_system"));
      assertEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("_system", fs.join(path, db._name()), db._id());
      checkCollections(tree, path, db._name());
      db._useDatabase("testName");
      assertNotEqual(-1, tree.indexOf("testName"));
      assertEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("testName", fs.join(path, db._name()), db._id());
      checkCollections(tree, path, db._name());
      db._useDatabase("😀");
      assertEqual(-1, tree.indexOf("😀"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("😀", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      db._useDatabase("ﻚﻠﺑ ﻞﻄﻴﻓ");
      assertEqual(-1, tree.indexOf("ﻚﻠﺑ ﻞﻄﻴﻓ"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("ﻚﻠﺑ ﻞﻄﻴﻓ", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpAllDatabasesWithOverwrite: function () {
      let path = fs.getTempFile();
      let args = ['--all-databases', 'true'];
      runDump(path, args, 0);

      // run the dump a second time, to overwrite all data in the target directory
      args.push('--overwrite');
      args.push('true');

      let tree = runDump(path, args, 0);
      db._useDatabase("maçã");
      assertEqual(-1, tree.indexOf("maçã"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("maçã", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      db._useDatabase("_system");
      assertNotEqual(-1, tree.indexOf("_system"));
      assertEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("_system", fs.join(path, db._name()), db._id());
      checkCollections(tree, path, db._name());
      db._useDatabase("testName");
      assertNotEqual(-1, tree.indexOf("testName"));
      assertEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("testName", fs.join(path, db._name()), db._id());
      checkCollections(tree, path, db._name());
      db._useDatabase("😀");
      assertEqual(-1, tree.indexOf("😀"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("😀", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      db._useDatabase("ﻚﻠﺑ ﻞﻄﻴﻓ");
      assertEqual(-1, tree.indexOf("ﻚﻠﺑ ﻞﻄﻴﻓ"));
      assertNotEqual(-1, tree.indexOf(db._id()));
      checkDumpJsonFile("ﻚﻠﺑ ﻞﻄﻴﻓ", fs.join(path, db._id()), db._id());
      checkCollections(tree, path, db._id());
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpCompressedEncryptedWithEnvelope: function () {
      if (!require("internal").isEnterprise()) {
        return;
      }

      let keyfile = fs.getTempFile();
      let path = fs.getTempFile();
      // 32 bytes of garbage
      fs.writeFileSync(keyfile, "01234567890123456789012345678901");

      let args = ['--compress-output', 'true', '--envelope', 'true', '--encryption.keyfile', keyfile, '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "aes-256-ctr");
      checkStructureFile(tree, path, false, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, false, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpCompressedEncryptedNoEnvelope: function () {
      if (!require("internal").isEnterprise()) {
        return;
      }

      let keyfile = fs.getTempFile();
      let path = fs.getTempFile();
      // 32 bytes of garbage
      fs.writeFileSync(keyfile, "01234567890123456789012345678901");

      let args = ['--compress-output', 'true', '--envelope', 'false', '--encryption.keyfile', keyfile, '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "aes-256-ctr");
      checkStructureFile(tree, path, false, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, false, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteUncompressedWithEnvelope: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'false', '--envelope', 'true', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, true, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'false', '--envelope', 'true', '--overwrite', 'true', '--collection', cn];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteUncompressedNoEnvelope: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'false', '--envelope', 'true', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, true, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'false', '--envelope', 'false', '--overwrite', 'true', '--collection', cn];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpCompressedEncrypted: function () {
      if (!require("internal").isEnterprise()) {
        return;
      }

      let keyfile = fs.getTempFile();
      let path = fs.getTempFile();
      // 32 bytes of garbage
      fs.writeFileSync(keyfile, "01234567890123456789012345678901");

      let args = ['--compress-output', 'true', '--encryption.keyfile', keyfile, '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "aes-256-ctr");
      checkStructureFile(tree, path, false, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, false, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteDisabled: function () {
      let path = fs.getTempFile();
      let args = ['--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");

      // second dump, without overwrite
      // this is expected to have an exit code of 1
      runDump(path, args, 1 /*exit code*/);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteCompressed: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'true', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'true', '--overwrite', 'true', '--collection', cn];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteUncompressed: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'false', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'false', '--overwrite', 'true', '--collection', cn];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, false, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteEncrypted: function () {
      if (!require("internal").isEnterprise()) {
        return;
      }

      let keyfile = fs.getTempFile();
      let path = fs.getTempFile();
      // 32 bytes of garbage
      fs.writeFileSync(keyfile, "01234567890123456789012345678901");

      let args = ['--compress-output', 'false', '--encryption.keyfile', keyfile, '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "aes-256-ctr");
      checkStructureFile(tree, path, false, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, false, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'false', '--encryption.keyfile', keyfile, '--overwrite', 'true', '--collection', cn];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "aes-256-ctr");
      checkStructureFile(tree, path, false, cn);
      checkDataFile(tree, path, /*split*/ false, false, true, false, cn);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteCompressedWithEncrypted: function () {
      if (!require("internal").isEnterprise()) {
        return;
      }

      let keyfile = fs.getTempFile();
      let path = fs.getTempFile();
      // 32 bytes of garbage
      fs.writeFileSync(keyfile, "01234567890123456789012345678901");

      let args = ['--compress-output', 'true', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);

      // second dump, which overwrites
      // this is expected to have an exit code of 1
      args = ['--compress-output', 'false', '--encryption.keyfile', keyfile, '--overwrite', 'true', '--collection', cn];
      runDump(path, args, 1 /*exit code*/);
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOverwriteOtherCompressed: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'true', '--collection', cn];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);

      // second dump, which overwrites
      args = ['--compress-output', 'true', '--overwrite', 'true', '--collection', cn + "Other"];
      tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);
      checkStructureFile(tree, path, true, cn + "Other");
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn + "Other");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpJustOneInvalidCollection: function () {
      let path = fs.getTempFile();
      let args = ['--collection', 'foobarbaz'];
      let tree = runDump(path, args, 1);
      checkEncryption(tree, path, "none");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpJustInvalidCollections: function () {
      let path = fs.getTempFile();
      let args = ['--collection', 'foobarbaz', '--collection', 'knarzknarzknarz'];
      let tree = runDump(path, args, 1);
      checkEncryption(tree, path, "none");
      fs.removeDirectoryRecursive(path, true);
    },

    testDumpOneValidCollection: function () {
      let path = fs.getTempFile();
      let args = ['--compress-output', 'true', '--collection', cn, '--collection', 'knarzknarzknarz'];
      let tree = runDump(path, args, 0);
      checkEncryption(tree, path, "none");
      checkStructureFile(tree, path, true, cn);
      checkDataFile(tree, path, /*split*/ false, true, false, true, cn);
      fs.removeDirectoryRecursive(path, true);
    },
    
    testDumpCollectionWithExtendedName: function () {
      let c = db._create(extendedName);
      try {
        let docs = [];
        for (let i = 0; i < 1000; ++i) {
          docs.push({_key: "test" + i});
        }
        c.insert(docs);

        let path = fs.getTempFile();
        let args = ['--compress-output', 'true', '--collection', extendedName];
        let tree = runDump(path, args, 0);
        checkEncryption(tree, path, "none");
        checkStructureFile(tree, path, true, extendedName, "", String(c._id));
        checkDataFile(tree, path, /*split*/ false, true, false, true, extendedName, String(c._id));
        fs.removeDirectoryRecursive(path, true);
      } finally {
        db._drop(extendedName);
      }
    },
  };
}

jsunity.run(dumpIntegrationSuite);

return jsunity.done();
