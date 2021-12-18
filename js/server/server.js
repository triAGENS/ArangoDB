'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / @brief server initialization
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2014 ArangoDB GmbH, Cologne, Germany
// / Copyright 2011-2014 triAGENS GmbH, Cologne, Germany
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
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Dr. Frank Celler
// / @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
// / @author Copyright 2011-2014, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

(function () {
  let internal = require('internal');
  let ArangoError = require('@arangodb').ArangoError;

  // check if --server.rest-server is disabled
  // in this case we do not (and should not) initialize and start Foxx
  let options = internal.options();
  let restServer = true;
  if (options.hasOwnProperty("server.rest-server")) {
    restServer = options["server.rest-server"];
  }

  // make sure the cache is filled:
  let col = internal.db._collections();
  // reload routing information
  if (restServer) {
    // the function name reloadRouting is misleading here, as it actually
    // only initializes/clears the local routing map, but doesn't rebuild
    // it.
    require('@arangodb/actions').reloadRouting();
  }

  // This script is also used by agents. Coords use a different script.
  // Make sure we only run these commands in single-server mode.
  if (internal.threadNumber === 0) {
    if (global.ArangoServerState.role() === 'SINGLE') {
      if (restServer) {
        // startup the foxx manager once
        require('@arangodb/foxx/manager')._startup();
      }

      // start the queue manager once
      try {
        require('@arangodb/foxx/queues/manager').run();
      } catch (err) {
        // ignore any shutdown errors
        if (!(err instanceof ArangoError) ||
            err.errorNum !== internal.errors.ERROR_SHUTTING_DOWN.code) {
          require("console").warn("unable to start Foxx queues manager: " + String(err));
        }
        // continue with the startup anyway!
      }
    }

    // check available versions
    require('@arangodb').checkAvailableVersions();
  }

  return true;
}());
