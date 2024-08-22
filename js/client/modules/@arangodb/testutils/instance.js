/* jshint strict: false, sub: true */
/* global print, arango */
'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
// / Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
// / @author Wilfried Goesgens
// //////////////////////////////////////////////////////////////////////////////

/* Modules: */
const _ = require('lodash');
const fs = require('fs');
const pu = require('@arangodb/testutils/process-utils');
const tu = require('@arangodb/testutils/test-utils');
const rp = require('@arangodb/testutils/result-processing');
const pm = require('@arangodb/testutils/portmanager');
const yaml = require('js-yaml');
const internal = require('internal');
const crashUtils = require('@arangodb/testutils/crash-utils');
const {sanHandler} = require('@arangodb/testutils/san-file-handler');
const crypto = require('@arangodb/crypto');
const ArangoError = require('@arangodb').ArangoError;
const debugGetFailurePoints = require('@arangodb/test-helper').debugGetFailurePoints;

/* Functions: */
const {
  toArgv,
  executeExternal,
  killExternal,
  statusExternal,
  statisticsExternal,
  suspendExternal,
  continueExternal,
  base64Encode,
  download,
  platform,
  time,
  wait,
  sleep
} = internal;

/* Constants: */
// const BLUE = internal.COLORS.COLOR_BLUE;
const CYAN = internal.COLORS.COLOR_CYAN;
const GREEN = internal.COLORS.COLOR_GREEN;
const RED = internal.COLORS.COLOR_RED;
const RESET = internal.COLORS.COLOR_RESET;
// const YELLOW = internal.COLORS.COLOR_YELLOW;
const IS_A_TTY = RED.length !== 0;


const abortSignal = 6;
const killSignal = 9;
const termSignal = 15;

let tcpdump;

let PORTMANAGER;

function getSockStatFile(pid) {
  try {
    return fs.read("/proc/" + pid + "/net/sockstat");
  } catch (e) {/* oops, process already gone? don't care. */ }
  return "";
}


const instanceType = {
  single: 'single',
  cluster: 'cluster'
};

const instanceRole = {
  single: 'single',
  agent: 'agent',
  dbServer: 'dbserver',
  coordinator: 'coordinator',
};

class agencyConfig {
  constructor(options, wholeCluster) {
    this.wholeCluster = wholeCluster;
    this.agencySize = options.agencySize;
    this.supervision = String(options.agencySupervision);
    this.waitForSync = false;
    if (options.agencyWaitForSync !== undefined) {
      this.waitForSync = options.agencyWaitForSync = false;
    }
    this.agencyInstances = [];
    this.agencyEndpoint = "";
    this.agentsLaunched = 0;
    this.urls = [];
    this.endpoints = [];
  }
  getStructure() {
    return {
      agencySize: this.agencySize,
      agencyInstances: this.agencyInstances.length,
      supervision: this.supervision,
      waitForSync: this.waitForSync,
      agencyEndpoint: this.agencyEndpoint,
      agentsLaunched: this.agentsLaunched,
      urls: this.urls,
      endpoints: this.endpoints
    };
  }
  setFromStructure(struct) {
    this.agencySize = struct['agencySize'];
    this.supervision = struct['supervision'];
    this.waitForSync = struct['waitForSync'];
    this.agencyEndpoint = struct['agencyEndpoint'];
    this.agentsLaunched = struct['agentsLaunched'];
    this.urls = struct['urls'];
    this.endpoints = struct['endpoints'];
  }
}

class instance {
  #pid = null;

  // / protocol must be one of ["tcp", "ssl", "unix"]
  constructor(options, instanceRole, addArgs, authHeaders, protocol, rootDir, restKeyFile, agencyConfig, tmpDir, mem) {
    this.id = null;
    this.pm = pm.getPortManager(options);
    this.options = options;
    this.instanceRole = instanceRole;
    this.rootDir = rootDir;
    this.protocol = protocol;

    this.args = {};
    for (const [key, value] of Object.entries(addArgs)) {
      if (key.search('extraArgs') >= 0) {
        let splitkey = key.split('.');
        if (splitkey.length !== 2) {
          if (splitkey[1] === this.instanceRole) {
            this.args[splitkey.slice(2).join('.')] = value;
          }
        } else {
          this.args[key] = value;
        }
      } else {
        this.args[key] = value;
      }
    }
    this.authHeaders = authHeaders;
    this.restKeyFile = restKeyFile;
    this.agencyConfig = agencyConfig;

    this.upAndRunning = false;
    this.suspended = false;
    this.message = '';
    this.port = '';
    this.url = '';
    this.endpoint = '';
    this.assertLines = [];
    this.useableMemory = mem;
    this.memProfCounter = 0;

    this.topLevelTmpDir = tmpDir;
    this.dataDir = fs.join(this.rootDir, 'data');
    this.appDir = fs.join(this.rootDir, 'apps');
    this.tmpDir = fs.join(this.rootDir, 'tmp');

    fs.makeDirectoryRecursive(this.dataDir);
    fs.makeDirectoryRecursive(this.appDir);
    fs.makeDirectoryRecursive(this.tmpDir);
    this.logFile = fs.join(rootDir, 'log');
    this.coreDirectory = this.rootDir;
    if (process.env.hasOwnProperty('COREDIR')) {
      this.coreDirectory = process.env['COREDIR'];
    }
    this.JWT = null;
    this.jwtFiles = null;
    this.sanHandler = new sanHandler('arangod', this.options);

    this._makeArgsArangod();

    this.name = instanceRole + ' - ' + this.port;
    this.exitStatus = null;
    this.serverCrashedLocal = false;
    this.netstat = {'in':{}, 'out': {}};
  }

  set pid(value) {
    if (this.#pid !== null) {
      this.processSanitizerReports();
    }
    this.#pid = value;
  }

  get pid() { return this.#pid; }

  getStructure() {
    return {
      name: this.name,
      instanceRole: this.instanceRole,
      message: this.message,
      rootDir: this.rootDir,
      protocol: this.protocol,
      authHeaders: this.authHeaders,
      restKeyFile: this.restKeyFile,
      agencyConfig: this.agencyConfig.getStructure(),
      upAndRunning: this.upAndRunning,
      suspended: this.suspended,
      port: this.port,
      url: this.url,
      endpoint: this.endpoint,
      dataDir: this.dataDir,
      appDir: this.appDir,
      tmpDir: this.tmpDir,
      logFile: this.logFile,
      args: this.args,
      pid: this.pid,
      id: this.id,
      JWT: this.JWT,
      jwtFiles: this.jwtFiles,
      exitStatus: this.exitStatus,
      serverCrashedLocal: this.serverCrashedLocal
    };
  }

  setFromStructure(struct, agencyConfig) {
    this.name = struct['name'];
    this.instanceRole = struct['instanceRole'];
    this.message = struct['message'];
    this.rootDir = struct['rootDir'];
    this.protocol = struct['protocol'];
    this.authHeaders = struct['authHeaders'];
    this.restKeyFile = struct['restKeyFile'];
    this.upAndRunning = struct['upAndRunning'];
    this.suspended = struct['suspended'];
    this.port = struct['port'];
    this.url = struct['url'];
    this.endpoint = struct['endpoint'];
    this.dataDir = struct['dataDir'];
    this.appDir = struct['appDir'];
    this.tmpDir = struct['tmpDir'];
    this.logFile = struct['logFile'];
    this.args = struct['args'];
    this.pid = struct['pid'];
    this.id = struct['id'];
    this.JWT = struct['JWT'];
    this.jwtFiles = struct['jwtFiles'];
    this.exitStatus = struct['exitStatus'];
    this.serverCrashedLocal = struct['serverCrashedLocal'];
  }

  isRole(compareRole) {
    // print(this.instanceRole + ' ==? ' + compareRole);
    return this.instanceRole === compareRole;
  }

  isAgent() {
    return this.instanceRole === instanceRole.agent;
  }
  isFrontend() {
    return ( (this.instanceRole === instanceRole.single) ||
             (this.instanceRole === instanceRole.coordinator) ||
             (this.instanceRole === instanceRole.failover)      );
  }

  checkNetstat(data) {
    let which = null;
    if (data.local.port === this.port) {
      which = this.netstat['in'];
    } else if (data.remote.port === this.port) {
      which = this.netstat['out'];
    }
    if (which !== null) {
      if (!which.hasOwnProperty(data.state)) {
        which[data.state] = 1;
      } else {
        which[data.state] += 1;
      }
    }
  }
  
  
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief arguments for testing (server)
  // //////////////////////////////////////////////////////////////////////////////

  _makeArgsArangod () {
    console.assert(this.tmpDir !== undefined);
    let endpoint;
    let bindEndpoint;
    if (!this.args.hasOwnProperty('server.endpoint')) {
      this.port = this.pm.findFreePort(this.options.minPort, this.options.maxPort);
      this.endpoint = this.protocol + '://127.0.0.1:' + this.port;
      bindEndpoint = this.endpoint;
      if (this.options.bindBroadcast) {
        bindEndpoint = this.protocol + '://0.0.0.0:' + this.port;
      }
    } else {
      this.endpoint = this.args['server.endpoint'];
      bindEndpoint = this.endpoint;
      this.port = this.endpoint.split(':').pop();
    }
    this.url = pu.endpointToURL(this.endpoint);

    if (this.appDir === undefined) {
      this.appDir = fs.getTempPath();
    }

    let config = 'arangod-' + this.instanceRole + '.conf';
    this.args = _.defaults(this.args, {
      'configuration': fs.join(pu.CONFIG_DIR, config),
      'define': 'TOP_DIR=' + pu.TOP_DIR,
      'javascript.app-path': this.appDir,
      'javascript.copy-installation': false,
      'http.trusted-origin': this.options.httpTrustedOrigin || 'all',
      'temp.path': this.tmpDir,
      'server.endpoint': bindEndpoint,
      'database.directory': this.dataDir,
      'temp.intermediate-results-path': fs.join(this.rootDir, 'temp-rocksdb-dir'),
      'log.file': this.logFile
    });
    if (this.options.forceOneShard) {
      this.args['cluster.force-one-shard'] = true;
    }
    if (require("@arangodb/test-helper").isEnterprise()) {
      this.args['arangosearch.columns-cache-limit'] = '100000';
    }
    if (this.options.auditLoggingEnabled) {
      this.args['audit.output'] = 'file://' + fs.join(this.rootDir, 'audit.log');
      this.args['server.statistics'] = false;
      this.args['foxx.queues'] = false;
    }

    if (this.protocol === 'ssl' && !this.args.hasOwnProperty('ssl.keyfile')) {
      this.args['ssl.keyfile'] = fs.join('etc', 'testing', 'server.pem');
    }
    if (this.options.encryptionAtRest && !this.args.hasOwnProperty('rocksdb.encryption-keyfile')) {
      this.args['rocksdb.encryption-keyfile'] = this.restKeyFile;
    }

    if (this.restKeyFile && !this.args.hasOwnProperty('server.jwt-secret')) {
      this.args['server.jwt-secret'] = this.restKeyFile;
    }
    else if (this.options.hasOwnProperty('jwtFiles')) {
      this.jwtFiles = this.options['jwtFiles'];
    // instanceInfo.authOpts['server.jwt-secret-folder'] = addArgs['server.jwt-secret-folder'];
    }


    if (this.options.hasOwnProperty("replicationVersion")) {
      this.args['database.default-replication-version'] = this.options.replicationVersion;
    }

    for (const [key, value] of Object.entries(this.options.extraArgs)) {
      let splitkey = key.split('.');
      if (splitkey.length !== 2) {
        if (splitkey[0] === this.instanceRole) {
          this.args[splitkey.slice(1).join('.')] = value;
        }
      } else {
        this.args[key] = value;
      }
    }

    let output = this.args.hasOwnProperty('log.output') ? this.args['log.output'] : [];
    if (typeof output === 'string') {
      output = [output];
    }

    if (this.options.verbose) {
      this.args['log.level'] = 'debug';
      output.push('-'); // make sure we always have a stdout appender
    } else if (this.options.noStartStopLogs) {
      // set the stdout appender to error only
      output.push('-;all=error');
      let logs = ['crash=info'];
      if (this.args['log.level'] !== undefined) {
        if (Array.isArray(this.args['log.level'])) {
          logs = logs.concat(this.args['log.level']);
        } else {
          logs.push(this.args['log.level']);
        }
      }
      this.args['log.level'] = logs;
    } else {
      output.push('-'); // make sure we always have a stdout appender
    }
    this.args['log.output'] = output;
    if (this.isAgent()) {
      this.args = Object.assign(this.args, {
        'agency.activate': 'true',
        'agency.size': this.agencyConfig.agencySize,
        'agency.wait-for-sync': this.agencyConfig.waitForSync,
        'agency.supervision': this.agencyConfig.supervision,
        'agency.my-address': this.protocol + '://127.0.0.1:' + this.port,
        // Sometimes for unknown reason the agency startup is too slow.
        // With this log level we might have a chance to see what is going on.
        'log.level': "agency=info",
      });
      if (!this.args.hasOwnProperty("agency.supervision-grace-period")) {
        this.args['agency.supervision-grace-period'] = '10.0';
      }
      if (!this.args.hasOwnProperty("agency.supervision-frequency")) {
        this.args['agency.supervision-frequency'] = '1.0';
      }
      this.agencyConfig.agencyInstances.push(this);
      if (this.agencyConfig.agencyInstances.length === this.agencyConfig.agencySize) {
        let l = [];
        this.agencyConfig.agencyInstances.forEach(agentInstance => {
          l.push(agentInstance.endpoint);
          this.agencyConfig.urls.push(agentInstance.url);
          this.agencyConfig.endpoints.push(agentInstance.endpoint);
        });
        this.agencyConfig.agencyInstances.forEach(agentInstance => {
          agentInstance.args['agency.endpoint'] = _.clone(l);
        });
        this.agencyConfig.agencyEndpoint = this.agencyConfig.agencyInstances[0].endpoint;
      }
    } else if (this.instanceRole === instanceRole.dbServer) {
      this.args = Object.assign(this.args, {
        'cluster.my-role':'PRIMARY',
        'cluster.my-address': this.args['server.endpoint'],
        'cluster.agency-endpoint': this.agencyConfig.agencyEndpoint
      });
    } else if (this.instanceRole === instanceRole.coordinator) {
      this.args = Object.assign(this.args, {
        'cluster.my-role': 'COORDINATOR',
        'cluster.my-address': this.args['server.endpoint'],
        'cluster.agency-endpoint': this.agencyConfig.agencyEndpoint,
        'foxx.force-update-on-startup': true
      });
      if (!this.args.hasOwnProperty('cluster.default-replication-factor')) {
        this.args['cluster.default-replication-factor'] = '2';
      }
    }
    if (this.args.hasOwnProperty('server.jwt-secret')) {
      this.JWT = this.args['server.jwt-secret'];
    }
    this.sanHandler.detectLogfiles(this.rootDir, this.topLevelTmpDir);
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief aggregates information from /proc about the SUT
  // //////////////////////////////////////////////////////////////////////////////
  _getProcessStats() {
    let processStats = statisticsExternal(this.pid);
    if (platform === 'linux') {
      let pidStr = "" + this.pid;
      let ioraw;
      let fn = fs.join('/', 'proc', pidStr, 'io');
      try {
        ioraw = fs.readBuffer(fn);
      } catch (x) {
        print("Proc FN gone: " + fn);
        print(x.stack);
        throw x;
      }
      /*
       * rchar: 1409391
       * wchar: 681539
       * syscr: 3303
       * syscw: 2969
       * read_bytes: 0
       * write_bytes: 0
       * cancelled_write_bytes: 0
       */
      let lineStart = 0;
      let maxBuffer = ioraw.length;
      for (let j = 0; j < maxBuffer; j++) {
        if (ioraw[j] === 10) { // \n
          const line = ioraw.utf8Slice(lineStart, j);
          lineStart = j + 1;
          let x = line.split(":");
          processStats[x[0]] = parseInt(x[1]);
        }
      }
      /* 
       * sockets: used 1272
       * TCP: inuse 27 orphan 0 tw 117 alloc 382 mem 25
       * UDP: inuse 19 mem 17
       * UDPLITE: inuse 0
       * RAW: inuse 0
       * FRAG: inuse 0 memory 0
       */
      ioraw = getSockStatFile(this.pid);
      ioraw.split('\n').forEach(line => {
        if (line.length > 0) {
          let x = line.split(":");
          let values = x[1].split(" ");
          for (let k = 1; k < values.length; k+= 2) {
            processStats['sockstat_' + x[0] + '_' + values[k]]
              = parseInt(values[k + 1]);
          }
        }
      });
    }
    return processStats;
  }
  getSockStat(preamble) {
    if (this.options.getSockStat && (platform === 'linux')) {
      let sockStat = preamble + this.pid + "\n";
      sockStat += getSockStatFile(this.pid);
      return sockStat;
    }
    return "";
  }

  cleanup() {
    if ((this.pid !== null) && (this.exitStatus === null)) {
      print(RED + Date() + "killing instance (again?) to make sure we can delete its files!" + RESET);
      this.terminateInstance();
    }
    if (this.options.extremeVerbosity) {
      print(CYAN + "cleaning up " + this.name + " 's Directory: " + this.rootDir + RESET);
    }
    if (fs.exists(this.rootDir)) {
      fs.removeDirectoryRecursive(this.rootDir, true);
    }
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief scans the log files for assert lines
  // //////////////////////////////////////////////////////////////////////////////

  readAssertLogLines (expectAsserts) {
    if (!fs.exists(this.logFile)) {
      if (fs.exists(this.rootDir)) {
        print(`readAssertLogLines: Logfile ${this.logFile} already gone.`);
      }
      return;
    }
    let size = fs.size(this.logFile);
    if (this.options.maxLogFileSize !== 0 && size > this.options.maxLogFileSize) {
      // File bigger 500k? this needs to be a bug in the tests.
      let err=`ERROR: ${this.logFile} is bigger than ${this.options.maxLogFileSize/1024}kB! - ${size/1024} kBytes!`;
      this.assertLines.push(err);
      print(RED + err + RESET);
      return;
    }
    try {
      const buf = fs.readBuffer(this.logFile);
      let lineStart = 0;
      let maxBuffer = buf.length;

      for (let j = 0; j < maxBuffer; j++) {
        if (buf[j] === 10) { // \n
          const line = buf.utf8Slice(lineStart, j);
          lineStart = j + 1;

          // scan for asserts from the crash dumper
          if (line.search('{crash}') !== -1) {
            if (!IS_A_TTY) {
              // else the server has already printed these:
              print("ERROR: " + line);
            }
            this.assertLines.push(line);
            if (!expectAsserts) {
              crashUtils.GDB_OUTPUT += line + '\n';
            }
          }
        }
      }
    } catch (ex) {
      let err="failed to read " + this.logFile + " -> " + ex;
      this.assertLines.push(err);
      print(RED+err+RESET);
    }
  }
  terminateInstance() {
    internal.removePidFromMonitor(this.pid);
    if (!this.hasOwnProperty('exitStatus')) {
      this.exitStatus = killExternal(this.pid, termSignal);
    }
    this.processSanitizerReports();
  }

  readImportantLogLines (logPath) {
    let fnLines = [];
    const buf = fs.readBuffer(fs.join(this.logFile));
    let lineStart = 0;
    let maxBuffer = buf.length;
    
    for (let j = 0; j < maxBuffer; j++) {
      if (buf[j] === 10) { // \n
        const line = buf.utf8Slice(lineStart, j);
        lineStart = j + 1;
        
        // filter out regular INFO lines, and test related messages
        let warn = line.search('WARNING about to execute:') !== -1;
        let info = line.search(' INFO ') !== -1;
        
        if (warn || info) {
          continue;
        }
        fnLines.push(line);
      }
    }
    return fnLines;
  }
  aggregateFatalErrors(currentTest) {
    if (this.assertLines.length > 0) {
      this.assertLines.forEach(line => {
        rp.addFailRunsMessage(currentTest, line);
      });
      this.assertLines = [];
    }
    if (this.serverCrashedLocal) {
      rp.addFailRunsMessage(currentTest, this.serverFailMessagesLocal);
      this.serverFailMessagesLocal = "";
    }
  }
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief executes a command, possible with valgrind
  // //////////////////////////////////////////////////////////////////////////////

  _executeArangod (moreArgs) {
    if (moreArgs && moreArgs.hasOwnProperty('server.jwt-secret')) {
      this.JWT = moreArgs['server.jwt-secret'];
    }

    let cmd = pu.ARANGOD_BIN;
    let args = _.defaults(moreArgs, this.args);
    let argv = [];
    if (this.options.valgrind) {
      let valgrindOpts = {};

      if (this.options.valgrindArgs) {
        valgrindOpts = this.options.valgrindArgs;
      }

      let testfn = this.options.valgrindFileBase;

      if (testfn.length > 0) {
        testfn += '_';
      }

      if (valgrindOpts.xml === 'yes') {
        valgrindOpts['xml-file'] = testfn + '.%p.xml';
      }

      valgrindOpts['log-file'] = testfn + '.%p.valgrind.log';
      argv = toArgv(valgrindOpts, true).concat([cmd]).concat(toArgv(args));
      cmd = this.options.valgrind;
    } else if (this.options.rr) {
      argv = [cmd].concat(toArgv(args));
      cmd = 'rr';
    } else {
      argv = toArgv(args);
    }

    if (this.options.extremeVerbosity) {
      print(Date() + ' starting process ' + cmd + ' with arguments: ' + JSON.stringify(argv));
    }

    let subEnv = this.sanHandler.getSanOptions();

    if ((this.useableMemory === undefined) && (this.options.memory !== undefined)){
      throw new Error(`${this.name} don't have planned memory though its configured!`);
    }
    if ((this.useableMemory !== 0) && (this.options.memory !== undefined)) {
      if (this.options.extremeVerbosity) {
        print(`appointed ${this.name} memory: ${this.useableMemory}`);
      }
      subEnv.push(`ARANGODB_OVERRIDE_DETECTED_TOTAL_MEMORY=${this.useableMemory}`);
    }
    subEnv.push(`ARANGODB_SERVER_DIR=${this.rootDir}`);
    let ret = executeExternal(cmd, argv, false, subEnv);
    return ret;
  }
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief starts an instance
  // /
  // //////////////////////////////////////////////////////////////////////////////

  startArango () {
    try {
      this.pid = this._executeArangod().pid;
      if (this.options.enableAliveMonitor) {
        internal.addPidToMonitor(this.pid);
      }
    } catch (x) {
      print(Date() + ' failed to run arangod - ' + JSON.stringify(x));

      throw x;
    }

    sleep(0.5);
    if (this.isAgent()) {
      this.agencyConfig.agentsLaunched += 1;
      if (this.agencyConfig.agentsLaunched === this.agencyConfig.agencySize) {
        this.agencyConfig.wholeCluster.checkClusterAlive();
      }
    }
  }
  launchInstance(moreArgs) {
    if (this.pid !== null) {
      print(RED + "can not re-launch when PID still there. " + this.name + " - " + this.pid);
      throw new Error("kill the instance before relaunching it!");
      return;
    }
    try {
      let args = {...this.args, ...moreArgs};
      /// TODO Y? Where?
      this.pid = this._executeArangod(args).pid;
    } catch (x) {
      print(Date() + ' failed to run arangod - ' + JSON.stringify(x) + " - " + JSON.stringify(this.getStructure()));

      throw x;
    }
    this.endpoint = this.args['server.endpoint'];
    this.url = pu.endpointToURL(this.endpoint);
    if (this.options.enableAliveMonitor) {
      internal.addPidToMonitor(this.pid);
    }
  };

  status(waitForExit) {
    let ret = statusExternal(this.pid, waitForExit);
    if (ret.status !== 'RUNNING') {
      this.processSanitizerReports();
    }
    return ret;
  }

  waitForExitAfterDebugKill() {
    if (this.pid === null) {
      return;
    }
    // Crashutils debugger kills our instance, but we neet to get
    // testing.js sapwned-PID-monitoring adjusted.
    print(this.name + " waiting for exit - " + this.pid);
    try {
      let ret = statusExternal(this.pid, false);
      // OK, something has gone wrong, process still alive. announce and force kill:
      if (ret.status !== "ABORTED") {
        print(RED + `was expecting the ${this.name} process ${this.pid} to be gone, but ${JSON.stringify(ret)}` + RESET);
        this.processSanitizerReports();
        killExternal(this.pid, abortSignal);
        print(statusExternal(this.pid, true));
      }
    } catch(ex) {
      print(ex);
    }
    this.pid = null;
    print('done');
  }

  waitForExit() {
    if (this.pid === null) {
      this.exitStatus = null;
      return;
    }
    this.exitStatus = statusExternal(this.pid, true);
    if (this.exitStatus.status !== 'TERMINATED') {
      this.processSanitizerReports();
      throw new Error(this.name + " didn't exit in a regular way: " + JSON.stringify(this.exitStatus));
    }
    this.exitStatus = null;
    this.pid = null;
  }

  restartOneInstance(moreArgs, unAuthOK) {
    if (unAuthOK === undefined) {
      unAuthOK = false;
    }
    const startTime = time();
    this.exitStatus = null;
    this.pid = null;
    this.upAndRunning = false;

    print(CYAN + Date()  + " relaunching: " + this.name + ', url: ' + this.url + RESET);
    this.launchInstance(moreArgs);
    while(true) {
      const reply = download(this.url + '/_api/version', '');

      if (!reply.error && reply.code === 200) {
        break;
      }
      if (unAuthOK && reply.code === 401) {
        break;
      }
      if (this.options.extremeVerbosity) {
        print(this.name + ' answered: ' + JSON.stringify(reply));
      }
      sleep(0.5);
      if (!this.checkArangoAlive()) {
        print("instance gone! " + this.name);
        this.pid = null;
        throw new Error("restart failed! " + this.name);
      }
    }
    print(CYAN + Date() + ' ' + this.name + ', url: ' + this.url + ', running again with PID ' + this.pid + RESET);
  }
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief periodic checks whether spawned arangod processes are still alive
  // //////////////////////////////////////////////////////////////////////////////
  checkArangoAlive () {
    if (this.pid === null) {
      return false;
    }
    let res = statusExternal(this.pid, false);
    if (res.status === 'NOT-FOUND') {
      print(`${Date()} ${this.name}: PID ${this.pid} missing on our list, retry?`);
      sleep(0.2);
      res = statusExternal(this.pid, false);
    }
    const running = res.status === 'RUNNING';
    if (!this.options.coreCheck && this.options.setInterruptable && !running) {
      print(`fatal exit of ${this.pid} arangod => ${JSON.stringify(res)}! Bye!`);
      pu.killRemainingProcesses({status: false});
      process.exit();
    }

    if (!running) {
      if (!this.hasOwnProperty('message')) {
        this.message = '';
      }
      let msg = ` ArangoD of role [${this.name}] with PID ${this.pid} is gone by: ${JSON.stringify(res)}`;
      print(Date() + msg + ':');
      this.message += (this.message.length === 0) ? '\n' : '' + msg + ' ';
      if (!this.hasOwnProperty('exitStatus') || (this.exitStatus === null)) {
        this.exitStatus = res;
      }
      print(this.getStructure());

      if (res.hasOwnProperty('signal') &&
          ((res.signal === 11) ||
           (res.signal === 6))) {
        msg = 'health Check Signal(' + res.signal + ') ';
        this.analyzeServerCrash(msg);
        this.serverCrashedLocal = true;
        this.message += msg;
        msg = " checkArangoAlive: Marking crashy";
        pu.serverCrashed = true;
        this.message += msg;
        print(Date() + msg + ' - ' + JSON.stringify(this.getStructure()));
        this.pid = null;
      }
    }
    return running;
  }

  isRunning() {
    let check = () => (this.exitStatus !== null) && (this.exitStatus.status === 'RUNNING');
    if (check()) {
      this.exitStatus = this.status(false);
      return check();
    }
    return false;
  }

  connect() {
    if (this.JWT) {
      return arango.reconnect(this.endpoint, '_system', 'root', '', true, this.JWT);
    } else {
      return arango.reconnect(this.endpoint, '_system', 'root', '', true);
    }
  }

  checkArangoConnection(count, overrideVerbosity=false) {
    this.endpoint = this.args['server.endpoint'];
    while (count > 0) {
      try {
        if (this.options.extremeVerbosity || overrideVerbosity) {
          print('tickeling ' + this.endpoint);
        }
        this.connect();
        return;
      } catch (e) {
        if (this.options.extremeVerbosity || overrideVerbosity) {
          print(`no... ${e.message}`);
        }
        sleep(0.5);
      }
      count --;
    }
    throw new Error(`unable to connect in ${count}s`);
  }
  getAgent(path, method, body = null) {
    let opts = {
      method: method
    };
    if (body === null) {
      body = (method === 'POST') ? '[["/"]]' : '';
    }

    if (this.args.hasOwnProperty('authOpts')) {
      opts['jwt'] = crypto.jwtEncode(this.authOpts['server.jwt-secret'], {'server_id': 'none', 'iss': 'arangodb'}, 'HS256');
    } else if (this.args.hasOwnProperty('server.jwt-secret')) {
      opts['jwt'] = crypto.jwtEncode(this.args['server.jwt-secret'], {'server_id': 'none', 'iss': 'arangodb'}, 'HS256');
    } else if (this.jwtFiles) {
      opts['jwt'] = crypto.jwtEncode(fs.read(this.jwtFiles[0]), {'server_id': 'none', 'iss': 'arangodb'}, 'HS256');
    }
    return download(this.url + path, body, opts);
  }

  dumpAgent(path, method, fn, dumpdir) {
    print('--------------------------------- '+ fn + ' -----------------------------------------------');
    let agencyReply = this.getAgent(path, method);
    if (agencyReply.code === 200) {
      if (fn === "agencyState") {
        fs.write(fs.join(dumpdir, fn + '_' + this.pid + ".json"), agencyReply.body);
      } else {
        let agencyValue = JSON.parse(agencyReply.body);
        fs.write(fs.join(dumpdir, fn + '_' + this.pid + ".json"), JSON.stringify(agencyValue, null, 2));
      }
    } else {
      print(agencyReply);
    }
  }

  killWithCoreDump (message) {
    if (this.pid == null) {
      return;
    }
    let pid = this.pid;
    if (this.options.enableAliveMonitor) {
      internal.removePidFromMonitor(this.pid);
    }
    this.getInstanceProcessStatus();
    this.serverCrashedLocal = true;
    if (this.pid === null) {
      this.pid = pid;
      print(`${RED}${Date()} instance already gone? ${this.name} ${JSON.stringify(this.exitStatus)}${RESET}`);
      this.analyzeServerCrash(`instance ${this.name} during force terminate server already dead? ${JSON.stringify(this.exitStatus)}`);
      this.pid = null;
    } else {
      print(`${RED}${Date()} attempting to generate crashdump of: ${this.name} ${JSON.stringify(this.exitStatus)}${RESET}`);
      crashUtils.generateCrashDump(pu.ARANGOD_BIN, this, this.options, message);
    }
  }

  aggregateDebugger () {
    crashUtils.aggregateDebugger(this, this.options);
    print(CYAN + Date() + this.name + ', url: ' + this.url + "unlisting our instance" + RESET);
    this.waitForExitAfterDebugKill();
  }
  // //////////////////////////////////////////////////////////////////////////////
  // / @brief commands a server to shut down via webcall
  // //////////////////////////////////////////////////////////////////////////////

  kill({signal = killSignal}) {
    print(CYAN + Date() +' killing ' + this.name + ', url: ' + this.url);

    if ((this.exitStatus === null) ||
      (this.exitStatus.status === 'RUNNING')) {
      if (this.options.enableAliveMonitor) {
        internal.removePidFromMonitor(this.pid);
      }
      this.exitStatus = killExternal(this.pid, signal);
      this.pid = null;
    } else {
      print(Date() + ' Server already dead, doing nothing.');
    }
  }

  shutdownArangod (forceTerminate) {
    if (this.pid == null) {
      print(CYAN + Date() + this.name + ', url: ' + this.url + ' already dead, doing nothing' + RESET);
      return;
    }
    print(CYAN + Date() +' stopping ' + this.name + ', pid ' + this.pid + ', url: ' + this.url + ', force terminate: ' + forceTerminate + ' ' + this.protocol + RESET);
    if (forceTerminate === undefined) {
      forceTerminate = false;
    }
    if (this.options.hasOwnProperty('server')) {
      print(Date() + ' running with external server');
      return;
    }

    if (this.options.valgrind) {
      this.waitOnServerForGC(60);
    }
    if (this.options.rr && forceTerminate) {
      forceTerminate = false;
      this.options.useKillExternal = true;
    }
    if (this.options.enableAliveMonitor) {
      internal.removePidFromMonitor(this.pid);
    }
    if ((this.exitStatus === null) ||
        (this.exitStatus.status === 'RUNNING')) {
      if (forceTerminate) {
        let sockStat = this.getSockStat(Date() + "Force killing - sockstat before: ");
        this.killWithCoreDump('shutdown timeout; instance forcefully KILLED because of fatal timeout in testrun ' + sockStat);
        this.pid = null;
      } else if (this.options.useKillExternal) {
        let sockStat = this.getSockStat("Shutdown by kill - sockstat before: ");
        this.exitStatus = killExternal(this.pid);
        this.pid = null;
        print(sockStat);
      } else if (this.protocol === 'unix') {
        let sockStat = this.getSockStat("Sock stat for: ");
        let reply = {code: 555};
        try {
          print(this.connect());
          reply = arango.DELETE_RAW('/_admin/shutdown');
        } catch(ex) {
          print(RED + 'while invoking shutdown via unix domain socket: ' + ex + RESET);
        };
        if ((reply.code !== 200) && // if the server should reply, we expect 200 - if not:
            !((reply.code === 500) &&
              (
                (reply.parsedBody === "Connection closed by remote") || // http connection
                  reply.parsedBody.includes('failed with #111')           // https connection
              ))) {
          this.serverCrashedLocal = true;
          print(Date() + ' Wrong shutdown response: ' + JSON.stringify(reply) + "' " + sockStat + " continuing with hard kill!");
          this.shutdownArangod(true);
        } else {
          this.processSanitizerReports();
          if (!this.options.noStartStopLogs) {
            print(sockStat);
          }
        }
        if (this.options.extremeVerbosity) {
          print(Date() + ' Shutdown response: ' + JSON.stringify(reply));
        }
      } else {
        const requestOptions = pu.makeAuthorizationHeaders(this.options, this.args, this.JWT);
        requestOptions.method = 'DELETE';
        requestOptions.timeout = 60; // 60 seconds hopefully are enough for getting a response
        if (!this.options.noStartStopLogs) {
          print(Date() + ' ' + this.url + '/_admin/shutdown');
        }
        let sockStat = this.getSockStat("Sock stat for: ");
        const reply = download(this.url + '/_admin/shutdown', '', requestOptions);
        if ((reply.code !== 200) && // if the server should reply, we expect 200 - if not:
            !((reply.code === 500) &&
              (
                (reply.message === "Connection closed by remote") || // http connection
                  reply.message.includes('failed with #111')           // https connection
              ))) {
          this.serverCrashedLocal = true;
          print(Date() + ' Wrong shutdown response: ' + JSON.stringify(reply) + "' " + sockStat + " continuing with hard kill!");
          this.shutdownArangod(true);
        }
        else if (!this.options.noStartStopLogs) {
          print(sockStat);
        }
        if (this.options.extremeVerbosity) {
          print(Date() + ' Shutdown response: ' + JSON.stringify(reply));
        }
      }
    } else {
      print(Date() + ' Server already dead, doing nothing.');
    }
  }

  waitForInstanceShutdown(timeout) {
    if (this.pid === null) {
      throw new Error(this.name + " already exited!");
    }
    if (this.options.enableAliveMonitor) {
      internal.removePidFromMonitor(this.pid);
    }
    while (timeout > 0) {
      this.exitStatus = statusExternal(this.pid, false);
      if (this.exitStatus.status === 'TERMINATED') {
        return true;
      }
      sleep(1);
      timeout--;
    }
    this.shutDownOneInstance({nonAgenciesCount: 1}, true, 0);
    crashUtils.aggregateDebugger(this, this.options);
    this.waitForExitAfterDebugKill();
    this.pid = null;
    return false;
  }

  shutDownOneInstance(counters, forceTerminate, timeout) {
    let shutdownTime = time();
    if (this.options.enableAliveMonitor) {
      internal.removePidFromMonitor(this.pid);
    }
    if (this.exitStatus === null) {
      this.shutdownArangod(forceTerminate);
      if (forceTerminate) {
        print(Date() + " FORCED shut down: " + JSON.stringify(this.getStructure()));
      } else {
        this.exitStatus = {
          status: 'RUNNING'
        };
        print(Date() + " Commanded shut down: " + JSON.stringify(this.getStructure()));
      }
      return true;
    }
    if (this.exitStatus.status === 'RUNNING') {
      this.exitStatus = statusExternal(this.pid, false);
    }
    if (this.exitStatus.status === 'RUNNING') {
      let localTimeout = timeout;
      if (this.isAgent()) {
        localTimeout = localTimeout + 60;
      }
      if ((time() - shutdownTime) > localTimeout) {
        this.agencyConfig.wholeCluster.dumpAgency();
        print(Date() + ' forcefully terminating ' + yaml.safeDump(this.getStructure()) +
              ' after ' + timeout + 's grace period; marking crashy.');
        this.serverCrashedLocal = true;
        counters.shutdownSuccess = false;
        this.killWithCoreDump('shutdown timeout; instance "' +
                              this.name +
                              '" forcefully KILLED after 60s');
        if (!this.isAgent()) {
          counters.nonAgenciesCount--;
        }
        return false;
      } else {
        return true;
      }
    } else if (this.exitStatus.status !== 'TERMINATED') {
      if (!this.isAgent()) {
        counters.nonAgenciesCount--;
      }
      if (this.exitStatus.hasOwnProperty('signal') || this.exitStatus.hasOwnProperty('monitor')) {
        this.analyzeServerCrash('instance "' + this.name + '" Shutdown - ' + this.exitStatus.signal);
        print(Date() + " shutdownInstance: Marking crashy - " + JSON.stringify(this.getStructure()));
        this.serverCrashedLocal = true;
        counters.shutdownSuccess = false;
      }
      crashUtils.stopProcdump(this.options, this);
    } else {
      if (!this.isAgent()) {
        counters.nonAgenciesCount--;
      }
      if (!this.options.noStartStopLogs) {
        print(Date() + ' Server "' + this.name + '" shutdown: Success: pid', this.pid);
      }
      crashUtils.stopProcdump(this.options, this);
      return false;
    }
  }

  getInstanceProcessStatus() {
    if (this.pid !== null) {
      this.exitStatus = statusExternal(this.pid, false);
      if (this.exitStatus.status !== 'RUNNING') {
        this.pid = null;
      }
    }
  }

  suspend() {
    if (this.suspended) {
      print(CYAN + Date() + ' NOT suspending "' + this.name + " again!" + RESET);
      return;
    }
    print(CYAN + Date() + ' suspending ' + this.name + ', url: ' + this.url + RESET);
    if (this.options.enableAliveMonitor) {
      internal.removePidFromMonitor(this.pid);
    }
    if (!suspendExternal(this.pid)) {
      throw new Error("Failed to suspend " + this.name);
    }
    this.suspended = true;
    return true;
  }

  resume() {
    if (!this.suspended) {
      print(CYAN + Date() + ' NOT resuming "' + this.name + " again!" + RESET);
      return;
    }
    print(CYAN + Date() + ' resuming ' + this.name + ', url: ' + this.url + RESET);
    if (!continueExternal(this.pid)) {
      throw new Error("Failed to resume " + this.name);
    }
    if (this.options.enableAliveMonitor) {
      internal.addPidToMonitor(this.pid);
    }
    this.suspended = false;
    return true;
  }

  // //////////////////////////////////////////////////////////////////////////////
  // / @brief the bad has happened, tell it the user and try to gather more
  // /        information about the incident. (arangod wrapper for the crash-utils)
  // //////////////////////////////////////////////////////////////////////////////
  analyzeServerCrash (checkStr) {
    if (this.exitStatus === null) {
      return 'Not yet launched!';
    }
    return crashUtils.analyzeCrash(pu.ARANGOD_BIN, this, this.options, checkStr);
  }

  getMemProfSnapshot(opts) {
    let fn = fs.join(this.rootDir, `${this.role}_${this.pid}_${this.memProfCounter}_.heap`);
    let heapdumpReply = download(this.url + '/_admin/status?memory=true', opts);
    if (heapdumpReply.code === 200) {
      fs.write(fn, heapdumpReply.body);
      print(CYAN + Date() + ` Saved ${fn}` + RESET);
    } else {
      print(RED + Date() + ` Acquiring Heapdump for ${fn} failed!` + RESET);
      print(heapdumpReply);
    }

    let fnMetrics = fs.join(this.rootDir, `${this.role}_${this.pid}_${this.memProfCounter}_.metrics`);
    let metricsReply = download(this.url + '/_admin/metrics/v2', opts);
    if (metricsReply.code === 200) {
      fs.write(fnMetrics, metricsReply.body);
      print(CYAN + Date() + ` Saved ${fnMetrics}` + RESET);
    } else if (metricsReply.code === 503) {
      print(RED + Date() + ` Acquiring metrics for ${fnMetrics} not possible!` + RESET);
    } else {
      print(RED + Date() + ` Acquiring metrics for ${fnMetrics} failed!` + RESET);
      print(metricsReply);
    }
    this.memProfCounter ++;
  }

  processSanitizerReports() {
    this.serverCrashedLocal |= this.sanHandler.fetchSanFileAfterExit(this.pid);
  }
}


exports.instance = instance;
exports.agencyConfig = agencyConfig;
exports.instanceType = instanceType;
exports.instanceRole = instanceRole;
exports.registerOptions = function(optionsDefaults, optionsDocumentation) {
  tu.CopyIntoObject(optionsDefaults, {
    'enableAliveMonitor': true,
    'maxLogFileSize': 500 * 1024,
    'skipLogAnalysis': true,
    'bindBroadcast': false,
    'getSockStat': false,
    'rr': false,
  });

  tu.CopyIntoList(optionsDocumentation, [
    ' SUT instance properties:',
    '   - `enableAliveMonitor`: checks whether spawned arangods disapears or aborts during the tests.',
    '   - `maxLogFileSize`: how big logs should be at max - 500k by default',
    "   - `skipLogAnalysis`: don't try to crawl the server logs",
    '   - `bindBroadcast`: whether to work with loopback or 0.0.0.0',
    '   - `rr`: if set to true arangod instances are run with rr',
    '   - `getSockStat`: on linux collect socket stats before shutdown',
    '   - `replicationVersion`: if set, define the default replication version. (Currently we have "1" and "2")',
  ]);
};
