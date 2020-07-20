/*jshint -W051:true, evil:true */
/*eslint-disable */
;(function () {
  'use strict'
  /*eslint-enable */

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief module "internal"
  // /
  // / @file
  // /
  // / DISCLAIMER
  // /
  // / Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
  // / @author Dr. Frank Celler
  // / @author Copyright 2010-2013, triAGENS GmbH, Cologne, Germany
  // //////////////////////////////////////////////////////////////////////////////

  var exports = require('internal');
  var console = require('console');
  var fs = require('fs');

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief db
  // //////////////////////////////////////////////////////////////////////////////

  exports.db = global.db;
  delete global.db;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoAnalyzers
  ////////////////////////////////////////////////////////////////////////////////

  exports.ArangoAnalyzers = global.ArangoAnalyzers;
  delete global.ArangoAnalyzers;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoCollection
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoCollection = global.ArangoCollection;
  delete global.ArangoCollection;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoView
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoView = global.ArangoView;
  delete global.ArangoView;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoUsers
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoUsers = global.ArangoUsers;
  delete global.ArangoUsers;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoGeneralGraphModule
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoGeneralGraphModule = global.ArangoGeneralGraphModule;
  delete global.ArangoGeneralGraphModule;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoGeneralGraphClass
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoGraph = global.ArangoGraph;
  delete global.ArangoGraph;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoSmartGraphClass
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoSmartGraph = global.ArangoSmartGraph;
  delete global.ArangoSmartGraph;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoDatabase
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoDatabase = global.ArangoDatabase;
  delete global.ArangoDatabase;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief ArangoQueryStreamCursor
  // //////////////////////////////////////////////////////////////////////////////

  exports.ArangoQueryStreamCursor = global.ArangoQueryStreamCursor;
  delete global.ArangoQueryStreamCursor;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief dispatcherThreads
  // //////////////////////////////////////////////////////////////////////////////

  exports.dispatcherThreads = global.DISPATCHER_THREADS;
  delete global.DISPATCHER_THREADS;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief frontendVersionCheck
  // //////////////////////////////////////////////////////////////////////////////

  exports.frontendVersionCheck = global.FE_VERSION_CHECK;
  delete global.FE_VERSION_CHECK;

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief reloadRouting
  // //////////////////////////////////////////////////////////////////////////////

  exports.reloadRouting = function () {
    require('@arangodb/actions').reloadRouting();
  };

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief resets engine in development mode
  // //////////////////////////////////////////////////////////////////////////////

  exports.resetEngine = function () {
    require('@arangodb/actions').reloadRouting();
  };

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief write-ahead log object
  // //////////////////////////////////////////////////////////////////////////////

  exports.wal = {
    flush: function () {
      return global.WAL_FLUSH.apply(null, arguments);
    },

    properties: function () {
      return global.WAL_PROPERTIES.apply(null, arguments);
    },

    transactions: function () {
      return global.WAL_TRANSACTIONS.apply(null, arguments);
    },

    waitForCollector: function () {
      return global.WAL_WAITCOLLECTOR.apply(null, arguments);
    }
  };

  // / @brief ttlStatistics
  if (global.SYS_TTL_STATISTICS) {
    exports.ttlStatistics = global.SYS_TTL_STATISTICS;
    delete global.SYS_TTL_STATISTICS;
  }

  // / @brief ttlProperties
  if (global.SYS_TTL_PROPERTIES) {
    exports.ttlProperties = global.SYS_TTL_PROPERTIES;
    delete global.SYS_TTL_PROPERTIES;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief defines an action
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEFINE_ACTION) {
    exports.defineAction = global.SYS_DEFINE_ACTION;
    delete global.SYS_DEFINE_ACTION;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief expose configuration
  // //////////////////////////////////////////////////////////////////////////////


  if (global.SYS_IS_FOXX_API_DISABLED) {
    exports.isFoxxApiDisabled = global.SYS_IS_FOXX_API_DISABLED;
    delete global.SYS_IS_FOXX_API_DISABLED;
  }

  if (global.SYS_IS_FOXX_STORE_DISABLED) {
    exports.isFoxxStoreDisabled = global.SYS_IS_FOXX_STORE_DISABLED;
    delete global.SYS_IS_FOXX_STORE_DISABLED;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief expose foxx manager
  // //////////////////////////////////////////////////////////////////////////////


  if (global.SYS_EXECUTE_FOXX_LOCKED) {
    exports.executeFoxxLocked = global.SYS_EXECUTE_FOXX_LOCKED;
    delete global.SYS_EXECUTE_FOXX_LOCKED;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief throw-collection-not-loaded
  // //////////////////////////////////////////////////////////////////////////////

  if (global.THROW_COLLECTION_NOT_LOADED) {
    exports.throwOnCollectionNotLoaded = global.THROW_COLLECTION_NOT_LOADED;
    delete global.THROW_COLLECTION_NOT_LOADED;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief autoload modules from database
  // //////////////////////////////////////////////////////////////////////////////

  // autoload specific modules
  exports.autoloadModules = function () {
    console.debug('autoloading actions');

    try {
      let modules = exports.db._collection('_modules');

      if (modules === null) {
        // _modules is an optional collection. if it does not exist,
        // we can simply go on and ignore it
        console.debug('autoloading actions finished, no _modules collection found');
        return;
      }

      modules = modules.byExample({ autoload: true }).toArray();

      modules.forEach(function (module) {
        // this module is only meant to be executed in one thread
        if (exports.threadNumber !== 0 && !module.perThread) {
          return;
        }

        console.debug('autoloading module: %s', module.path);

        try {
          // require a module
          if (module.path !== undefined) {
            require(module.path);
          }

          // execute a user function
          else if (module.func !== undefined) {
            /*eslint-disable */
            var func = new Function(module.func)
            /*eslint-enable */
            func();
          }
        } catch (err) {
          console.error('error while loading startup module "%s": %s', module.name || module.path, String(err));
        }
      });
    } catch (err) {
      console.error('error while loading startup modules: %s', String(err));
    }

    console.debug('autoloading actions finished');
  };

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief serverStatistics
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_SERVER_STATISTICS) {
    exports.serverStatistics = global.SYS_SERVER_STATISTICS;
    delete global.SYS_SERVER_STATISTICS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief processStatistics
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_PROCESS_STATISTICS) {
    exports.thisProcessStatistics = global.SYS_PROCESS_STATISTICS;
    exports.processStatistics = global.SYS_PROCESS_STATISTICS;
    delete global.SYS_PROCESS_STATISTICS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief whether or not Statistics are enabled
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_ENABLED_STATISTICS) {
    exports.enabledStatistics = global.SYS_ENABLED_STATISTICS;
    delete global.SYS_ENABLED_STATISTICS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief clientStatistics
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_CLIENT_STATISTICS) {
    exports.clientStatistics = global.SYS_CLIENT_STATISTICS;
    delete global.SYS_CLIENT_STATISTICS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief httpStatistics
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_HTTP_STATISTICS) {
    exports.httpStatistics = global.SYS_HTTP_STATISTICS;
    delete global.SYS_HTTP_STATISTICS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief getCurrentRequest
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_GET_CURRENT_REQUEST) {
    exports.getCurrentRequest = global.SYS_GET_CURRENT_REQUEST;
    delete global.SYS_GET_CURRENT_REQUEST;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief getCurrentResponse
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_GET_CURRENT_RESPONSE) {
    exports.getCurrentResponse = global.SYS_GET_CURRENT_RESPONSE;
    delete global.SYS_GET_CURRENT_RESPONSE;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief reloads the AQL user functions
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_EXECUTE_GLOBAL_CONTEXT_FUNCTION) {
    exports.reloadAqlFunctions = function () {
      global.SYS_EXECUTE_GLOBAL_CONTEXT_FUNCTION('reloadAql');
      require('@arangodb/aql').reload();
    };
  } else {
    exports.reloadAqlFunctions = function () {
      require('@arangodb/aql').reload();
    };
  }

  // replication
  if (global.REPLICATION_LOGGER_STATE) {
    exports.getStateReplicationLogger = global.REPLICATION_LOGGER_STATE;
    delete global.REPLICATION_LOGGER_STATE;
  }

  if (global.REPLICATION_LOGGER_TICK_RANGES) {
    exports.tickRangesReplicationLogger = global.REPLICATION_LOGGER_TICK_RANGES;
    delete global.REPLICATION_LOGGER_TICK_RANGES;
  }

  if (global.REPLICATION_LOGGER_FIRST_TICK) {
    exports.firstTickReplicationLogger = global.REPLICATION_LOGGER_FIRST_TICK;
    delete global.REPLICATION_LOGGER_FIRST_TICK;
  }

  if (global.REPLICATION_APPLIER_CONFIGURE) {
    // TODO: remove this export from the internal module
    exports.configureReplicationApplier = global.REPLICATION_APPLIER_CONFIGURE;
  }

  if (global.REPLICATION_APPLIER_START) {
    // TODO: remove this export from the internal module
    exports.startReplicationApplier = global.REPLICATION_APPLIER_START;
  }

  if (global.REPLICATION_APPLIER_STOP) {
    // TODO: remove this export from the internal module
    exports.stopReplicationApplier = global.REPLICATION_APPLIER_STOP;
  }

  if (global.REPLICATION_APPLIER_STATE) {
    // TODO: remove this export from the internal module
    exports.getStateReplicationApplier = global.REPLICATION_APPLIER_STATE;
  }

  if (global.REPLICATION_APPLIER_STATE_ALL) {
    // TODO: remove this export from the internal module
    exports.getStateReplicationApplierAll = global.REPLICATION_APPLIER_STATE_ALL;
  }

  if (global.REPLICATION_APPLIER_FORGET) {
    // TODO: remove this export from the internal module
    exports.forgetStateReplicationApplier = global.REPLICATION_APPLIER_FORGET;
  }

  if (global.REPLICATION_SYNCHRONIZE) {
    // TODO: remove this export from the internal module
    exports.synchronizeReplication = global.REPLICATION_SYNCHRONIZE;
  }

  if (global.REPLICATION_SERVER_ID) {
    exports.serverId = global.REPLICATION_SERVER_ID;
    delete global.REPLICATION_SERVER_ID;
  }
  
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief wait for index selectivity estimate sync
  // //////////////////////////////////////////////////////////////////////////////
  if (global.WAIT_FOR_ESTIMATOR_SYNC) {
    exports.waitForEstimatorSync = global.WAIT_FOR_ESTIMATOR_SYNC;
    delete global.WAIT_FOR_ESTIMATOR_SYNC;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief loadStartup
  // //////////////////////////////////////////////////////////////////////////////

  exports.loadStartup = function (path) {
    return exports.load(fs.join(exports.startupPath, path));
  };

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief raw request body
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_RAW_REQUEST_BODY) {
    const $_RAW_BODY_BUFFER = Symbol.for('@arangodb/request.rawBodyBuffer');
    const getRawBodyBuffer = global.SYS_RAW_REQUEST_BODY;
    exports.rawRequestBody = function (req) {
      return req[$_RAW_BODY_BUFFER] || getRawBodyBuffer(req);
    };
    delete global.SYS_RAW_REQUEST_BODY;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief request parts
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_REQUEST_PARTS) {
    exports.requestParts = global.SYS_REQUEST_PARTS;
    delete global.SYS_REQUEST_PARTS;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief default replication factor
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEFAULT_REPLICATION_FACTOR_SYSTEM) {
    exports.DEFAULT_REPLICATION_FACTOR_SYSTEM = global.SYS_DEFAULT_REPLICATION_FACTOR_SYSTEM;
    delete global.SYS_DEFAULT_REPLICATION_FACTOR_SYSTEM;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief returns if we are an Enterprise Edition or not
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_IS_ENTERPRISE) {
    exports.isEnterprise = global.SYS_IS_ENTERPRISE;
    delete global.SYS_IS_ENTERPRISE;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief createQueue
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_CREATE_QUEUE) {
    exports.createQueue = global.SYS_CREATE_QUEUE;
    delete global.SYS_CREATE_QUEUE;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief deleteQueue
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DELETE_QUEUE) {
    exports.deleteQueue = global.SYS_DELETE_QUEUE;
    delete global.SYS_DELETE_QUEUE;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief registerTask
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_REGISTER_TASK) {
    exports.registerTask = global.SYS_REGISTER_TASK;
    delete global.SYS_REGISTER_TASK;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief unregisterTask
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_UNREGISTER_TASK) {
    exports.unregisterTask = global.SYS_UNREGISTER_TASK;
    delete global.SYS_UNREGISTER_TASK;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief getTasks
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_GET_TASK) {
    exports.getTask = global.SYS_GET_TASK;
    delete global.SYS_GET_TASK;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief getShardDistrbiution
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_CLUSTER_SHARD_DISTRIBUTION) {
    exports.getShardDistribution = global.SYS_CLUSTER_SHARD_DISTRIBUTION;
    delete global.SYS_CLUSTER_SHARD_DISTRIBUTION;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief getCollectionShardDistrbiution
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_CLUSTER_COLLECTION_SHARD_DISTRIBUTION) {
    exports.getCollectionShardDistribution = global.SYS_CLUSTER_COLLECTION_SHARD_DISTRIBUTION;
    delete global.SYS_CLUSTER_COLLECTION_SHARD_DISTRIBUTION;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugTerminate
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_TERMINATE) {
    exports.debugTerminate = global.SYS_DEBUG_TERMINATE;
    delete global.SYS_DEBUG_TERMINATE;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugSetFailAt
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_SET_FAILAT) {
    exports.debugSetFailAt = global.SYS_DEBUG_SET_FAILAT;
    delete global.SYS_DEBUG_SET_FAILAT;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugRemoveFailAt
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_REMOVE_FAILAT) {
    exports.debugRemoveFailAt = global.SYS_DEBUG_REMOVE_FAILAT;
    delete global.SYS_DEBUG_REMOVE_FAILAT;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugShouldFailAt
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_SHOULD_FAILAT) {
    exports.debugShouldFailAt = global.SYS_DEBUG_SHOULD_FAILAT;
    delete global.SYS_DEBUG_SHOULD_FAILAT;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugClearFailAt
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_CLEAR_FAILAT) {
    exports.debugClearFailAt = global.SYS_DEBUG_CLEAR_FAILAT;
    delete global.SYS_DEBUG_CLEAR_FAILAT;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief debugCanUseFailAt
  // //////////////////////////////////////////////////////////////////////////////

  if (global.SYS_DEBUG_CAN_USE_FAILAT) {
    exports.debugCanUseFailAt = global.SYS_DEBUG_CAN_USE_FAILAT;
    delete global.SYS_DEBUG_CAN_USE_FAILAT;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief replicationFactor(s) and number of shards
  // //////////////////////////////////////////////////////////////////////////////

  if (global.DEFAULT_REPLICATION_FACTOR) {
    exports.defaultReplicationFactor = global.DEFAULT_REPLICATION_FACTOR;
    delete global.DEFAULT_REPLICATION_FACTOR;
  }
  if (global.MIN_REPLICATION_FACTOR) {
    exports.minReplicationFactor = global.MIN_REPLICATION_FACTOR;
    delete global.MIN_REPLICATION_FACTOR;
  }
  if (global.MAX_REPLICATION_FACTOR) {
    exports.maxReplicationFactor = global.MAX_REPLICATION_FACTOR;
    delete global.MAX_REPLICATION_FACTOR;
  }
  if (global.MAX_NUMBER_OF_SHARDS) {
    exports.maxNumberOfShards = global.MAX_NUMBER_OF_SHARDS;
    delete global.MAX_NUMBER_OF_SHARDS;
  }
  if (global.FORCE_ONE_SHARD) {
    exports.forceOneShard = global.FORCE_ONE_SHARD;
    delete global.FORCE_ONE_SHARD;
  }

  // /////////////////////////////////////////////////////////////////////////////
  // / @brief whether or not clustering is enabled
  // /////////////////////////////////////////////////////////////////////////////

  exports.isCluster = function () {
    let role = global.ArangoServerState.role();
    return (role !== undefined && role !== 'SINGLE' && role !== 'AGENT');
  };

}());
