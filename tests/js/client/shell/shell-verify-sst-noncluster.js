/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global fail, assertTrue, assertFalse, assertEqual, assertNotEqual, arango */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
// /
// / The Programs (which include both the software and documentation) contain
// / proprietary information of ArangoDB GmbH; they are provided under a license
// / agreement containing restrictions on use and disclosure and are also
// / protected by copyright, patent and other intellectual and industrial
// / property laws. Reverse engineering, disassembly or decompilation of the
// / Programs, except to the extent required to obtain interoperability with
// / other independently created software or as specified by law, is prohibited.
// /
// / It shall be the licensee's responsibility to take all appropriate fail-safe,
// / backup, redundancy, and other measures to ensure the safe use of
// / applications if the Programs are used for purposes such as nuclear,
// / aviation, mass transit, medical, or other inherently dangerous applications,
// / and ArangoDB GmbH disclaims liability for any damages caused by such use of
// / the Programs.
// /
// / This software is the confidential and proprietary information of ArangoDB
// / GmbH. You shall not disclose such confidential and proprietary information
// / and shall use it only in accordance with the terms of the license agreement
// / you entered into with ArangoDB GmbH.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Jan Steemann
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const internal = require('internal');
const arangodb = require('@arangodb');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const db = arangodb.db;
const isEnterprise = require("internal").isEnterprise();
const { executeExternalAndWaitWithSanitizer } = require('@arangodb/test-helper');

function verifySstSuite() {
  'use strict';
  const dirs = global.instanceManager.arangods.filter((s) => s.instanceRole === 'single').map((s) => s.rootDir);
  assertEqual(1, dirs.length);

  const cn = 'UnitTests';
  const arangod = pu.ARANGOD_BIN;

  assertTrue(fs.isFile(arangod), "arangod not found!");

  return {
    setUp: function () {
      let c = db._create(cn);
      let docs = [];
      for (let i = 0; i < 100000; ++i) {
        docs.push({ value1: i, value2: "testi" + i });
        if (docs.length === 2000) {
          c.insert(docs);
          docs = [];
        }
      }
    },

    tearDown: function () {
      db._drop(cn);

      dirs.forEach((d) => {
        // remove all the hotbackups that we created, because they will consume a
        // lot of disk space
        try {
          let path = fs.join(d, 'data', 'backups');
          if (fs.exists(path)) {
            fs.removeDirectoryRecursive(path);
          }
        } catch (err) {
          require("console").warn("unable to remove hotbackups from base directory " + d, err);
        }
      });
    },

    testVerify: function () {
      // This will a hot backup on the server.
      // note: the hotbackup copies away the .sst files to a backup
      // directory. we need this separate directory in order to run
      // --rocksdb.verify-sst on it. we cannot run the verification
      // on the original RocksDB directory as RocksDB may then move
      // .sst files around while we are verifying them.
      let res = arango.POST_RAW("/_admin/execute", `return require("internal").createHotbackup({});`);
      assertFalse(res.error, res);
      assertEqual(200, res.code, res);
      assertTrue(res.parsedBody.nrFiles > 0, res);
  
      const verifySsts = (dir) => {
        const args = [
          '-c', 'none',
          '--javascript.enabled', 'false',
          '--database.directory', dir,
          '--rocksdb.verify-sst', 'true',
          '--server.rest-server', 'false',
        ];

        // call ArangoDB with `--rocksdb.verify-sst true` and check exit code
        const actualRc = executeExternalAndWaitWithSanitizer(arangod, args, 'shell-verify-sst-noncluster');
        assertTrue(actualRc.hasOwnProperty("exit"), actualRc);
        assertEqual(0, actualRc.exit, actualRc);
      };
      
      // find backup directory
      let verified = false;
      dirs.forEach((d) => {
        let path = fs.join(d, 'data', 'backups');
        if (fs.exists(path)) {
          let files = fs.listTree(path).filter((fn) => fn.match(/\.sst$/));
          assertTrue(files.length > 0, files);
          verifySsts(path);
          verified = true;
        }
      });
      assertTrue(verified, dirs);
    },
  };

}

if (isEnterprise) {
  // we can only take hot backups in enterprise - thus the
  // test can only run in the enterprise version
  jsunity.run(verifySstSuite);
}

return jsunity.done();
