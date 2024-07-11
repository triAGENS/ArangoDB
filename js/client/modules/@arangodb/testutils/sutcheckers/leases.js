/* jshint strict: false, sub: true */
/* global print db arango */
'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Business Source License 1.1 (the "License");
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     https://github.com/arangodb/arangodb/blob/devel/LICENSE
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Michael Hackstein
// //////////////////////////////////////////////////////////////////////////////

// //////////////////////////////////////////////////////////////////////////////
// / @brief checks no leases were left on the SUT by tests
// //////////////////////////////////////////////////////////////////////////////

const tu = require('@arangodb/testutils/test-utils');

// //////////////////////////////////////////////////////////////////////////////
// / @brief checks that no new views were left on the SUT.
// //////////////////////////////////////////////////////////////////////////////
exports.checker = class {
  constructor(runner) {
    this.runner = runner;
    this.name = 'leases';
  }
  setUp() {
    return true;
  }
  runCheck(te) {
    if (!require("internal").isCluster()) {
      // Only cluster needs this check
      return true;
    }
    if (this.runner.isFailed(te)) {
      // We already failed, let's not bother with checks
      return false;
    }
    // We try the following up to 3 times.
    // There is a chance that we hit the historian writing
    // statistics at one of the calls, so we give it
    // time to complete and then look again.
    for (let tries = 0; tries < 3; ++tries) {
      const leases = arango.GET_RAW("/_admin/leases?type=all");
      if (leases.code !== 200) {
        this.runner.setResult(te, false, {
          status: false,
          message: `Failed to check on left-over leases, request code: ${leases.code}, message:  "${leases.errorMessage}".`
        });
        return false;
      }
      const body = leases.parsedBody.result;
      if (typeof body !== 'object' || Array.isArray(body) || body === null) {
        this.runner.setResult(te, false, {
          status: false,
          message: `Failed to check on left-over leases, non-expected body: ${JSON.stringify(leases.parsedBody, null, 2)}".`
        });
        return false;
      }

      const leftOverLeases = [];
      for (const [server, {leasedFromRemote, leasedToRemote}] of Object.entries(body)) {
        for (const [peer, leases] of Object.entries(leasedFromRemote)) {
          for (const l of leases) {
            leftOverLeases.push(`${server} still leases from ${peer}: ${l}`);
          }
        }
        for (const [peer, leases] of Object.entries(leasedToRemote)) {
          for (const l of leases) {
            leftOverLeases.push(`${server} still leased to ${peer}: ${l}`);
          }
        }
      }
      if (leftOverLeases.length === 0) {
        // Everything aborted
        return true;
      }
      if (tries === 2) {
        // We tried 2 times, but still
        // some leases are not properly aborted!
        // This indicates leftovers.
        this.runner.setResult(te, false, {
          status: false,
          message: `Not all leases are removed: ${JSON.stringify(leftOverLeases, null, 2)}".`
        });
        return false;
      }
      // Wait a bit to let the historian complete.
      // Should be triggered very rarely.
      require("internal").wait(0.5);
    }
    // Unreachable code
    this.runner.setResult(te, false, {
      status: false,
      message: `Logic error in leases checker. This code should not be reachable.`
    });
    return false;
  }
};
