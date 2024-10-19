/*jshint globalstrict:false, strict:false */
/*global assertEqual, assertNotEqual */

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2023-2023 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const tasks = require("@arangodb/tasks");
const internal = require("internal");

function RecreateDatabaseSuite() {
    const tasksCompleted = () => {
        return 0 === tasks.get().filter((task) => {
            return (task.id.match(/^UnitTest/) || task.name.match(/^UnitTest/));
        }).length;
    };

    const waitForTasks = () => {
        const time = internal.time;
        const start = time();
        while (!tasksCompleted()) {
            if (time() - start > 300) { // wait for 5 minutes maximum
                fail("Timeout after 5 minutes");
            }
            internal.sleep(0.5);
        }
    };
    const threadsEach = 3;
    const iterations = 15;
    const dbName = "UnitTestDatabase";

    /**
     * This test is a chaos test.
     * We try to enforce a previous deadlock
     * which could onyl show up under load, and with a
     * bad ordering of events when dropping and recreating of a database works in parallel
     * This test is best-effort test to trigger, as many components need to get into bad timing to fail.
     */
    const createDatabaseCode = `
      let db = require("internal").db;
      for (let i = 0; i < ${iterations}; ++i) {
        require("internal").print("Create iteration " + i);
        try {
          db._createDatabase("${dbName}");
          require("internal").print("Create iteration " + i + " success");
        } catch (e) {
          require("internal").print("Create iteration " + i + " failed: " + JSON.stringify(e));
        }
      }
    `;

    const dropDatabaseCode = `
      let db = require("internal").db;
      for (let i = 0; i < ${iterations}; ++i) {
        require("internal").print("Drop iteration " + i);
        try {
          db._dropDatabase("${dbName}");
          require("internal").print("Drop iteration " + i + " success");
        } catch (e) {
          require("internal").print("Drop iteration " + i + " failed: " + JSON.stringify(e));
        }
      }
    `;

    return {
        setUp: () => {
            db._createDatabase(dbName);
        },
        tearDown: () => {
            tasks.get().forEach(function(task) {
                if (task.id.match(/^UnitTest/) || task.name.match(/^UnitTest/)) {
                    try {
                        tasks.unregister(task);
                    } catch (err) {
                    }
                }
            });
        },

        testCreateDropInParallel: function() {
            for (let i = 0; i < threadsEach; ++i) {
                tasks.register({ name: `UnitTestsCreateDatabase${i}`, command: createDatabaseCode });
                tasks.register({ name: `UnitTestsDropDatabase${i}`, command: dropDatabaseCode });
            }

            // wait for insertion tasks to complete
            waitForTasks();
        }
    }

}

jsunity.run(RecreateDatabaseSuite);

return jsunity.done();