/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, arango */

// //////////////////////////////////////////////////////////////////////////////
// / @brief ArangoTransaction sTests
// /
// /
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
const {assertTrue, assertFalse, assertEqual, assertNotEqual, assertInstanceOf} = jsunity.jsUnity.assertions;
const internal = require('internal');
const arangodb = require('@arangodb');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const db = arangodb.db;
const isCluster = require("internal").isCluster();
const { executeExternalAndWaitWithSanitizer } = require('@arangodb/test-helper');
const dbs = [{"name": "maçã", "id": "9999994", "isUnicode": true}, {
  "name": "cachorro",
  "id": "9999995",
  "isUnicode": false
}, {"name": "testName", "id": "9999996", "isUnicode": false}, {
  "name": "😀",
  "id": "9999997",
  "isUnicode": true
}, {"name": "かわいい犬", "id": "9999998"}, {"name": "ﻚﻠﺑ ﻞﻄﻴﻓ", "id": "9999999", "isUnicode": true}];
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

function createCollectionStructureFile(path, cn) {
  let fn = fs.join(path, cn + ".structure.json");
  fs.write(fn, JSON.stringify({
    indexes: [],
    parameters: {
      name: cn,
      numberOfShards: 3,
      type: 2
    }
  }));
}

function createCollectionDataFile(data, path, cn, split) {
  const prefix = cn + "_" + require("@arangodb/crypto").md5(cn);
  let write = (data, fn) => {
    fs.write(fs.join(path, fn), data.map((d) => JSON.stringify(d)).join('\n'));
  };

  if (split) {
    const n = data.length;
    let id = 0; // file number
    let s = 0;
    for (let i = 0; i <= n; ++i) {
      if (i - s >= (n / 5) || i === n) {
        write(data.slice(s, i), prefix + "." + (id++) + ".data.json");
        s = i;
      }
    }
  } else {
    write(data, prefix + ".data.json");
  }
}

function createCollectionFiles(path, cn, split) {
  createCollectionStructureFile(path, cn);
  let data = [];
  for (let i = 0; i < 1000; ++i) {
    data.push({type: 2300, data: {_key: "test" + i, value: i}});
  }
  createCollectionDataFile(data, path, cn, /*split*/ false);
  return data;
}

function createDumpJsonFile(path, databaseName, id) {
  let fn = fs.join(path, "dump.json");
  fs.write(fn, JSON.stringify({
    database: databaseName,
    properties: {
      name: databaseName,
      id: id
    }
  }));
}

function restoreIntegrationSuite() {
  'use strict';
  const cn = 'UnitTestsRestore';
  const arangorestore = pu.ARANGORESTORE_BIN;

  assertTrue(fs.isFile(arangorestore), "arangorestore not found!");

  let addConnectionArgs = function (args) {
    let endpoint = arango.getEndpoint().replace(/\+vpp/, '').replace(/^http:/, 'tcp:').replace(/^https:/, 'ssl:').replace(/^vst:/, 'tcp:').replace(/^h2:/, 'tcp:');
    args.push('--server.endpoint');
    args.push(endpoint);
    if (args.indexOf("--all-databases") === -1 && args.indexOf("--server.database") === -1) {
      args.push('--server.database');
      args.push(arango.getDatabaseName());
    }
    args.push('--server.username');
    args.push(arango.connectedUser());
  };

  let runRestore = function (path, args, rc) {
    args.push('--input-directory');
    args.push(path);
    addConnectionArgs(args);
    const actualRc = executeExternalAndWaitWithSanitizer(arangorestore, args, 'shell-restore-integration');
    assertTrue(actualRc.hasOwnProperty("exit"), actualRc);
    assertEqual(rc, actualRc.exit, actualRc);
  };

  return {

    setUp: function () {
      db._drop(cn);
    },

    tearDown: function () {
      db._drop(cn);
      db._databases().forEach((database) => {
        if (database !== "_system") {
          db._dropDatabase(database);
        }
      });
    },

    testRestoreAutoIncrementKeyGenerator: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 1,
          type: 2,
          keyOptions: {type: "autoincrement", lastValue: 12345, increment: 3, offset: 19}
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let p = c.properties();
      assertEqual("autoincrement", p.keyOptions.type);
      assertEqual(12345, p.keyOptions.lastValue);
      assertEqual(3, p.keyOptions.increment);
      assertEqual(19, p.keyOptions.offset);

      let lastValue = p.keyOptions.lastValue;
      for (let i = 0; i < 10; ++i) {
        c.insert({});
        p = c.properties();
        let newLastValue = p.keyOptions.lastValue;
        if (!isCluster) {
          assertTrue(newLastValue > lastValue);
        }
        lastValue = newLastValue;
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestorePaddedKeyGenerator: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2,
          keyOptions: {type: "padded", lastValue: 12345}
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let p = c.properties();
      assertEqual("padded", p.keyOptions.type);
      assertEqual(12345, p.keyOptions.lastValue);

      let lastValue = p.keyOptions.lastValue;
      for (let i = 0; i < 10; ++i) {
        c.insert({});
        p = c.properties();
        let newLastValue = p.keyOptions.lastValue;
        assertTrue(newLastValue > lastValue);
        lastValue = newLastValue;
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithSchema: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          schema: validatorJson,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 1000; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value1: i, value2: "abc"}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(doc.value1, i);
        assertEqual(doc.value2, "abc");
      }
      const colProperties = db[cn].properties();
      assertTrue(colProperties.hasOwnProperty("schema"));
      const schema = colProperties.schema;
      assertEqual(schema, validatorJson);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithRepeatedDocuments: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 1000; ++i) {
        // will generate keys such as test0, test0, test1, test1 etc.
        data.push({type: 2300, data: {_key: "test" + Math.floor(i / 2), value: i, overwrite: (i % 2 === 1)}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length / 2, c.count());
      for (let i = 0; i < data.length / 2; ++i) {
        let doc = c.document("test" + i);
        assertEqual((i * 2) + 1, doc.value);
        assertTrue(doc.overwrite);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreInsertRemove: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 10; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value: i, old: true}});
      }
      for (let i = 0; i < 6; ++i) {
        data.push({type: 2302, key: "test" + i});
      }
      for (let i = 4; i < 7; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value: i * 2, overwrite: true}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true', '--overwrite', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn), count = c.count();
      assertEqual(6, count);
      for (let i = 0; i < 4; ++i) {
        assertFalse(c.exists("test" + i));
      }
      for (let i = 4; i < 7; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i * 2, doc.value);
        assertTrue(doc.overwrite);
        assertFalse(doc.hasOwnProperty('old'));
      }
      for (let i = 8; i < 10; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
        assertTrue(doc.old);
        assertFalse(doc.hasOwnProperty('overwrite'));
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithComputedValues: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");
      let numShards = 1;
      if (isCluster) {
        numShards = 3;
      }
      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: numShards,
          type: 2,
          computedValues: [{
            name: "value3",
            expression: "RETURN CONCAT(@doc.value1, '+', @doc.value2)",
            computeOn: ["insert"],
            overwrite: false,
            failOnWarning: false
          }, {
            name: "value4",
            expression: "RETURN CONCAT(@doc.value2, ' ', @doc.value1)",
            computeOn: ["insert"],
            overwrite: true,
            failOnWarning: false
          }]
        }
      }));

      let data = [];
      for (let i = 0; i < 1000; ++i) {
        data.push({
          type: 2300,
          data: {_key: "test" + i, value1: i, value2: "abc", value3: i + "+abc", value4: "abc " + i}
        });
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(doc.value1, i);
        assertEqual(doc.value2, "abc");
        assertEqual(doc.value3, doc.value1 + "+" + doc.value2);
        assertEqual(doc.value4, doc.value2 + " " + doc.value1);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithLineBreaksInData: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 1000; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value: i}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithEnvelopesWithDumpJsonFile: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 5000; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value: i}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      fn = fs.join(path, "dump.json");
      fs.write(fn, JSON.stringify({
        useEnvelopes: true
      }));

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithEnvelopesNoDumpJsonFile: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 5000; ++i) {
        data.push({type: 2300, data: {_key: "test" + i, value: i}});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithAllDatabasesUnicodeNoCollections: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let args = ['--all-databases', 'true', '--create-database', 'true'];
      try {
        let subdir = "";
        dbs.forEach((database) => {
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
          }
          let dbPath = fs.join(path, subdir);
          fs.makeDirectory(dbPath);
          createDumpJsonFile(dbPath, database["name"], database["id"]);
        });
        runRestore(path, args, 0);
        dbs.forEach((database) => {
          db._useDatabase(database["name"]);
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
            assertEqual(subdir, db._name());
          }
          let data = JSON.parse(fs.readFileSync(fs.join(path, fs.join(subdir, "dump.json"))).toString());
          assertEqual(data.properties.name, db._name());
        });
        fs.removeDirectoryRecursive(path, true);
      } finally {
        db._useDatabase("_system");
      }
    },

    testRestoreWithSomeAlreadyExistingDatabasesUnicodeNoCollections: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let args = ['--all-databases', 'true', '--create-database', 'true'];
      try {
        let subdir = "";
        dbs.forEach((database, index) => {
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
          }
          let dbPath = fs.join(path, subdir);
          fs.makeDirectory(dbPath);
          if (index % 2 === 0) {
            db._createDatabase(database["name"]);
          }
          createDumpJsonFile(dbPath, database["name"], database["id"]);
        });
        assertTrue(db._databases().length > 2);
        runRestore(path, args, 0);
        dbs.forEach((database) => {
          db._useDatabase(database["name"]);
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
            assertEqual(subdir, db._name());
          }
          let data = JSON.parse(fs.readFileSync(fs.join(path, fs.join(subdir, "dump.json"))).toString());
          assertEqual(data.properties.name, db._name());
        });
        fs.removeDirectoryRecursive(path, true);
      } finally {
        db._useDatabase("_system");
      }
    },

    testRestoreCollectionDatabasesUnicode: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      try {
        let subdir = "";
        dbs.forEach((database) => {
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
          }
          let dbPath = fs.join(path, subdir);
          fs.makeDirectory(dbPath);
          createDumpJsonFile(dbPath, database["name"], database["id"]);
          let data = createCollectionFiles(dbPath, cn);
          let args = ['--collection', cn, '--import-data', 'true', '--create-database', 'true', '--server.database', database["name"]];
          runRestore(dbPath, args, 0);
          db._useDatabase(database["name"]);
          let c = db._collection(cn);
          assertEqual(data.length, c.count());
          for (let i = 0; i < data.length; ++i) {
            let doc = c.document("test" + i);
            assertEqual(i, doc.value);
          }
        });
        fs.removeDirectoryRecursive(path, true);
      } finally {
        db._useDatabase("_system");
      }
    },

    testRestoreCollectionWithAllDatabasesUnicode: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      try {
        let subdir = "";
        dbs.forEach((database) => {
          if (database["isUnicode"]) {
            subdir = database["id"];
          } else {
            subdir = database["name"];
          }
          let dbPath = fs.join(path, subdir);
          fs.makeDirectory(dbPath);
          createDumpJsonFile(dbPath, database["name"], database["id"]);
          createCollectionFiles(dbPath, cn);
        });
        let args = ['--collection', cn, '--import-data', 'true', '--create-database', 'true', '--all-databases', 'true'];
        runRestore(path, args, 0);
        dbs.forEach((database) => {
          db._useDatabase(database["name"]);
          let c = db._collection(cn);
          assertEqual(1000, c.count());
          for (let i = 0; i < 1000; ++i) {
            let doc = c.document("test" + i);
            assertEqual(i, doc.value);
          }
        });
        fs.removeDirectoryRecursive(path, true);
      } finally {
        db._useDatabase("_system");
      }
    },
    
    testRestoreSplitFiles: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 5000; ++i) {
        data.push({_key: "test" + i, value: i});
      }

      createCollectionDataFile(data, path, cn, /*split*/ true);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithoutEnvelopesWithDumpJsonFile: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 5000; ++i) {
        data.push({_key: "test" + i, value: i});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      fn = fs.join(path, "dump.json");
      fs.write(fn, JSON.stringify({
        useEnvelopes: false
      }));

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithoutEnvelopesNoDumpJsonFile: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let data = [];
      for (let i = 0; i < 5000; ++i) {
        data.push({_key: "test" + i, value: i});
      }

      createCollectionDataFile(data, path, cn, /*split*/ false);

      let args = ['--collection', cn, '--import-data', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertEqual(data.length, c.count());
      for (let i = 0; i < data.length; ++i) {
        let doc = c.document("test" + i);
        assertEqual(i, doc.value);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithoutUsesRevisions: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertTrue(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithUsesRevisionsFalse: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          usesRevisionsAsDocumentIds: false,
          name: cn,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertFalse(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreWithUsesRevisionsTrue: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          usesRevisionsAsDocumentIds: true,
          name: cn,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertTrue(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreNumericGloballyUniqueId: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          globallyUniqueId: "123456789012",
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      assertNotEqual("123456789012", c.properties().globallyUniqueId);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreIndexesOldFormat: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "0", fields: ["_key"], type: "primary", unique: true},
            {id: "95", fields: ["loc"], type: "geo", geoJson: false},
            {id: "295", fields: ["value"], type: "skiplist", sparse: true},
          ],
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(3, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("geo", indexes[1].type);
      assertEqual(["loc"], indexes[1].fields);
      assertFalse(indexes[1].geoJson);
      assertEqual("skiplist", indexes[2].type);
      assertEqual(["value"], indexes[2].fields);

      // test if the indexes work
      for (let i = 0; i < 100; ++i) {
        c.insert({_key: "test" + i, value: 42});
      }
      for (let i = 0; i < 100; ++i) {
        assertEqual("test" + i, c.document("test" + i)._key);
      }
      let result = db._query("FOR doc IN " + cn + " FILTER doc.value == 42 RETURN doc").toArray();
      assertEqual(100, result.length);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreIndexesOldFormatGeo1: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "95", fields: ["loc"], type: "geo1", geoJson: false},
          ],
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(2, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("geo", indexes[1].type);
      assertEqual(["loc"], indexes[1].fields);
      assertFalse(indexes[1].geoJson);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreIndexesOldFormatGeo2: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "95", fields: ["a", "b"], type: "geo2", geoJson: false},
          ],
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(2, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("geo", indexes[1].type);
      assertEqual(["a", "b"], indexes[1].fields);
      assertFalse(indexes[1].geoJson);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreIndexesFulltextLengthZero: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "95", fields: ["text"], type: "fulltext", minLength: 0},
          ],
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(2, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("fulltext", indexes[1].type);
      assertEqual(["text"], indexes[1].fields);
      assertEqual(indexes[1].minLength, 1);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreIndexesNewFormat: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [
          {id: "95", fields: ["loc"], type: "geo", geoJson: false},
          {id: "295", fields: ["value"], type: "skiplist", sparse: true},
        ],
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(3, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("geo", indexes[1].type);
      assertEqual(["loc"], indexes[1].fields);
      assertFalse(indexes[1].geoJson);
      assertEqual("skiplist", indexes[2].type);
      assertEqual(["value"], indexes[2].fields);

      // test if the indexes work
      for (let i = 0; i < 100; ++i) {
        c.insert({_key: "test" + i, value: 42});
      }
      for (let i = 0; i < 100; ++i) {
        assertEqual("test" + i, c.document("test" + i)._key);
      }
      let result = db._query("FOR doc IN " + cn + " FILTER doc.value == 42 RETURN doc").toArray();
      assertEqual(100, result.length);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreEdgeIndexOldFormat: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "0", fields: ["_key"], type: "primary", unique: true},
            {id: "1", fields: ["_from", "_to"], type: "edge"},
            {id: "95", fields: ["value"], type: "hash"},
          ],
          name: cn,
          numberOfShards: 3,
          type: 3 // edge collection
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(3, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("edge", indexes[1].type);
      assertEqual(["_from", "_to"], indexes[1].fields);
      assertEqual("hash", indexes[2].type);
      assertEqual(["value"], indexes[2].fields);

      // test if the indexes work
      for (let i = 0; i < 100; ++i) {
        c.insert({_key: "test" + i, _from: "v/" + i, _to: "v/" + i, value: 42});
      }
      for (let i = 0; i < 100; ++i) {
        assertEqual("test" + i, c.document("test" + i)._key);
      }
      for (let i = 0; i < 100; ++i) {
        let inEdges = c.inEdges("v/" + i);
        assertEqual(1, inEdges.length);
        assertEqual("test" + i, inEdges[0]._key);

        let outEdges = c.outEdges("v/" + i);
        assertEqual(1, outEdges.length);
        assertEqual("test" + i, outEdges[0]._key);
      }
      let result = db._query("FOR doc IN " + cn + " FILTER doc.value == 42 RETURN doc").toArray();
      assertEqual(100, result.length);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreEdgeIndexWrongCollectionType: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          indexes: [
            {id: "0", fields: ["_key"], type: "primary", unique: true},
            {id: "1", fields: ["_from", "_to"], type: "edge"},
          ],
          name: cn,
          numberOfShards: 3,
          type: 2 // document collection
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(1, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);

      // test if the index works
      for (let i = 0; i < 100; ++i) {
        c.insert({_key: "test" + i});
      }
      for (let i = 0; i < 100; ++i) {
        assertEqual("test" + i, c.document("test" + i)._key);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreEdgeIndexNewFormat: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [], // no edge index here, as it is an automatic index!
        parameters: {
          name: cn,
          numberOfShards: 3,
          type: 3 // edge collection
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let indexes = c.indexes();
      assertEqual(2, indexes.length);
      assertEqual("primary", indexes[0].type);
      assertEqual(["_key"], indexes[0].fields);
      assertEqual("edge", indexes[1].type);
      assertEqual(["_from", "_to"], indexes[1].fields);

      // test if the indexes work
      for (let i = 0; i < 100; ++i) {
        c.insert({_key: "test" + i, _from: "v/" + i, _to: "v/" + i, value: 42});
      }
      for (let i = 0; i < 100; ++i) {
        assertEqual("test" + i, c.document("test" + i)._key);
      }
      for (let i = 0; i < 100; ++i) {
        let inEdges = c.inEdges("v/" + i);
        assertEqual(1, inEdges.length);
        assertEqual("test" + i, inEdges[0]._key);

        let outEdges = c.outEdges("v/" + i);
        assertEqual(1, outEdges.length);
        assertEqual("test" + i, outEdges[0]._key);
      }
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreRegressionDistributeShardsLike: function () {
      const collectionsJson = [
        {"parameters": {"name": "Comment_hasTag_Tag_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Comment_Smart", "type": 2, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Forum_hasMember_Person", "type": 3}},
        {"parameters": {"name": "Forum_hasTag_Tag", "type": 3}},
        {"parameters": {"name": "Forum", "type": 2}},
        {"parameters": {"name": "Organisation", "type": 2}},
        {"parameters": {"name": "Person_hasCreated_Comment_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Person_hasCreated_Post_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Person_hasInterest_Tag", "type": 3}},
        {"parameters": {"name": "Person_knows_Person_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Person_likes_Comment_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Person_likes_Post_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Person_Smart", "type": 2}},
        {"parameters": {"name": "Person_studyAt_University", "type": 3}},
        {"parameters": {"name": "Person_workAt_Company", "type": 3}},
        {"parameters": {"name": "Place", "type": 2}},
        {"parameters": {"name": "Post_hasTag_Tag_Smart", "type": 3, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "Post_Smart", "type": 2, "distributeShardsLike": "Person_Smart"}},
        {"parameters": {"name": "TagClass", "type": 2}},
        {"parameters": {"name": "Tag", "type": 2}},
      ];
      // satisfy the file format requirements by adding indexes
      collectionsJson.forEach(col => {
        col.indexes = [];
      });

      const path = fs.getTempFile();
      fs.makeDirectory(path);

      const dbName = 'UnitTestRestoreRegressionDb';
      db._createDatabase(dbName);
      createDumpJsonFile(path, dbName);

      for (const colJson of collectionsJson) {
        const colName = colJson.parameters.name;
        const fn = fs.join(path, colName + ".structure.json");
        fs.write(fn, JSON.stringify(colJson));
      }

      const args = ['--server.database', dbName, '--import-data', 'false'];
      runRestore(path, args, 0);

      db._useDatabase(dbName);
      for (const colJson of collectionsJson) {
        const col = db._collection(colJson.parameters.name);
        assertInstanceOf(arangodb.ArangoCollection, col);
        assertEqual(colJson.parameters.name, col.name());
        assertEqual(colJson.parameters.type, col.type());
        if (isCluster) {
          assertEqual(colJson.parameters.distributeShardsLike, col.properties().distributeShardsLike);
        }
      }
      fs.removeDirectoryRecursive(path, true);
      db._useDatabase("_system");
    },

    testRestoreEnableRevisionTreesTrue: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          type: 2
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false', '--enable-revision-trees', 'true'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertTrue(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreEnableRevisionTreesFalse1: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          type: 2,
          usesRevisionsAsDocumentIds: true,
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false', '--enable-revision-trees', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertTrue(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },

    testRestoreEnableRevisionTreesFalse2: function () {
      let path = fs.getTempFile();
      fs.makeDirectory(path);
      let fn = fs.join(path, cn + ".structure.json");

      fs.write(fn, JSON.stringify({
        indexes: [],
        parameters: {
          name: cn,
          type: 2,
          usesRevisionsAsDocumentIds: false,
          syncByRevision: false,
        }
      }));

      let args = ['--collection', cn, '--import-data', 'false', '--enable-revision-trees', 'false'];
      runRestore(path, args, 0);

      let c = db._collection(cn);
      let props = c.properties();
      assertTrue(props.hasOwnProperty("syncByRevision"));
      assertFalse(props.syncByRevision);
      fs.removeDirectoryRecursive(path, true);
    },
  };
}

jsunity.run(restoreIntegrationSuite);

return jsunity.done();
