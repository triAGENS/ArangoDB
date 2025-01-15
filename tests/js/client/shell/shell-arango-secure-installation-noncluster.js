/* jshint globalstrict:false, strict:false, maxlen: 200 */
/* global assertTrue, assertEqual */

// //////////////////////////////////////////////////////////////////////////////
// / @brief arango-secure-installation tests
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

let jsunity = require('jsunity');
let internal = require('internal');
let fs = require('fs');
let pu = require('@arangodb/testutils/process-utils');
const { executeExternalAndWaitWithSanitizer } = require('@arangodb/test-helper');

function arangoSecureInstallationSuite () {
  'use strict';

  let oldPasswd;

  const arangoSecureInstallation = pu.ARANGO_SECURE_INSTALLATION_BIN;

  assertTrue(fs.isFile(arangoSecureInstallation), "arango-secure-installation not found!");

  return {
    setUpAll: function () {
      oldPasswd = internal.env['ARANGODB_DEFAULT_ROOT_PASSWORD'];
    },
    
    tearDownAll: function () {
      internal.env['ARANGODB_DEFAULT_ROOT_PASSWORD'] = oldPasswd;
    },

    setUp: function () {
      delete internal.env['ARANGODB_DEFAULT_ROOT_PASSWORD'];
    },
    
    testInvokeArangoSecureInstallationWithoutPassword: function () {
      let platform = internal.platform;
      if (platform !== 'linux') {
        // test is currently only supported on Linux
        return;
      }

      let path = fs.getTempFile();
      // database directory
      fs.makeDirectory(path);

      // set no password for the database
      try {
        // invoke arango-secure-installation without password. this will fail
        let actualRc = executeExternalAndWaitWithSanitizer(arangoSecureInstallation, [path], 'shell-arango-secure-installation-noncluster-1');
        assertTrue(actualRc.hasOwnProperty("exit"));
        assertEqual(1, actualRc.exit);
      } finally {
        try {
          fs.removeDirectory(path);
        } catch (err) {}
      }
    },

    testInvokeArangoSecureInstallationWithPassword: function () {
      let platform = internal.platform;
      if (platform !== 'linux') {
        // test is currently only supported on Linux
        return;
      }

      let path = fs.getTempFile();
      // database directory
      fs.makeDirectory(path);

      // set an initial password for the database
      internal.env['ARANGODB_DEFAULT_ROOT_PASSWORD'] = 'haxxmann';
      try {
        let args = [path];
        // invoke arango-secure-installation with password. this must succeed
        let actualRc = executeExternalAndWaitWithSanitizer(arangoSecureInstallation, [path], 'shell-arango-secure-installation-noncluster-2');
        assertTrue(actualRc.hasOwnProperty("exit"));
        assertEqual(0, actualRc.exit);
      } finally {
        try {
          fs.removeDirectory(path);
        } catch (err) {}
      }
    },
    
  };
}

jsunity.run(arangoSecureInstallationSuite);

return jsunity.done();
