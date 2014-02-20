/*jslint indent: 2, nomen: true, maxlen: 100, sloppy: true, vars: true, white: true, plusplus: true */
/*global ArangoServerState, require, exports */

////////////////////////////////////////////////////////////////////////////////
/// @brief General unittest framework
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Copyright 2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_UnitTest
/// @brief framework to perform unittests
///
/// This function gets one or two arguments, the first describes which tests
/// to perform and the second is an options object. For `which` the following
/// values are allowed:
///
///   - "all": do all tests
///   - "config"
///   - "boost"
///   - "shell_server"
///   - "shell_server_ahuacatl"
///   - "http_server"
///   - "ssl_server"
///   - "shell_client"
///   - "dump"
///   - "arangob"
///   - "import"
///   - "upgrade"
///   - "dfdb"
///   - "foxx-manager"
///   - "authentication"
///   - "authentication-parameters
///   - "single": convenience to execute a single test file
///
/// The following properties of `options` are defined:
///
///   - `force`: if set to true the tests are continued even if one fails
///   - `skipBoost`: if set to true the boost unittests are skipped
///   - `skipGeo`: if set to true the geo index tests are skipped
///   - `skipAhuacatl`: if set to true the ahuacatl tests are skipped
///   - `skipRanges`: if set to true the ranges tests are skipped
///   - `valgrind`: if set to true the arangods are run with the valgrind
///     memory checker
///   - `cluster`: if set to true the tests are run with the coordinator
///     of a small local cluster
///   - `test`: path to single test to execute for "single" test target
///   - `skipServer`: flag for "single" test target to skip the server test
///   - `skipClient`: flag for "single" test target to skip the client test
////////////////////////////////////////////////////////////////////////////////

var _ = require("underscore");

var testFuncs = {};
var print = require("internal").print;
var fs = require("fs");
var download = require("internal").download;
var wait = require("internal").wait;
var executeExternal = require("internal").executeExternal;
var killExternal = require("internal").killExternal;
var statusExternal = require("internal").statusExternal;
var base64Encode = require("internal").base64Encode;

var PortFinder = require("org/arangodb/cluster").PortFinder;
var Planner = require("org/arangodb/cluster").Planner;
var Kickstarter = require("org/arangodb/cluster").Kickstarter;

var optionsDefaults = { "cluster": false,
                        "valgrind": false,
                        "force": true,
                        "skipBoost": false,
                        "skipGeo": false,
                        "skipAhuacatl": false,
                        "skipRanges": false,
                        "username": "root",
                        "password": "",
                        "test": undefined,
                        "skipServer": false,
                        "skipClient": false };

function findTopDir () {
  var topDir = fs.normalize(fs.makeAbsolute("."));
  if (!fs.exists("3rdParty") && !fs.exists("arangod") && 
      !fs.exists("arangosh") && !fs.exists("UnitTests")) {
    throw "Must be in ArangoDB topdir to execute unit tests.";
  }
  return topDir;
}
  
function makeTestingArgs () {
  var topDir = findTopDir();
  return [ "--configuration",                  "none",
           "--database.maximal-journal-size",  "1048576",
           "--database.force-sync-properties", "false",
           "--javascript.gc-interval",         "1",
           "--javascript.app-path",            fs.join(topDir,"js","apps"),
           "--javascript.startup-directory",   fs.join(topDir,"js"),
           "--ruby.action-directory",          fs.join(topDir,"mr","actions"),
           "--ruby.modules-path", 
             fs.join(topDir,"mr","server","modules")+":"+
             fs.join(topDir,"mr","common","modules"),
           "--server.threads",                 "4",
           "--server.disable-authentication",  "true",
           "--server.allow-use-database",      "true" ];
}

function makeTestingArgsClient (options) {
  var topDir = findTopDir();
  return [ "--configuration",                  "none",
           "--javascript.startup-directory",   fs.join(topDir,"js"),
           "--no-colors",
           "--quiet",
           "--server.username",                options.username,
           "--server.password",                options.password ];
}

function makeAuthorisationHeaders (options) {
  return {"headers":
            {"Authorization": "Basic " + base64Encode(options.username+":"+
                                                      options.password)}};
}

function startInstance (protocol, options) {
  // protocol must be one of ["tcp", "ssl", "unix"]
  var topDir = findTopDir();
  var instanceInfo = {};
  instanceInfo.topDir = topDir;
  var tmpDataDir = fs.getTempFile();
  fs.makeDirectoryRecursive(tmpDataDir);
  instanceInfo.tmpDataDir = tmpDataDir;

  var endpoint;
  var pos;
  if (options.cluster) {
    // FIXME: protocol and valgrind currently ignored!
    var p = new Planner({"numberOfDBservers":2, 
                         "numberOfCoordinators":1,
                         "dispatchers": 
                           {"me":{"endpoint":"tcp://localhost:",
                                  "arangodExtraArgs":makeTestingArgs(),
                                  "username": "root",
                                  "password": ""}}
                        });
    instanceInfo.kickstarter = new Kickstarter(p.getPlan());
    instanceInfo.kickstarter.launch();
    var runInfo = instanceInfo.kickstarter.runInfo;
    var roles = runInfo[runInfo.length-1].roles;
    var endpoints = runInfo[runInfo.length-1].endpoints;
    pos = roles.indexOf("Coordinator");
    endpoint = endpoints[pos];
  }
  else {   // single instance mode
    // We use the PortFinder to find a free port for our subinstance,
    // to this end, we have to fake a dummy dispatcher:
    var dispatcher = {endpoint: "tcp://localhost:", avoidPorts: {}, id: "me"};
    var pf = new PortFinder([8529],dispatcher);
    var port = pf.next();
    instanceInfo.port = port;
    var args = makeTestingArgs();
    args.push("--server.endpoint");
    endpoint = protocol+"://127.0.0.1:"+port;
    args.push(endpoint);
    args.push("--database.directory");
    args.push(fs.join(tmpDataDir,"data"));
    fs.makeDirectoryRecursive(fs.join(tmpDataDir,"data"));
    args.push("--log.file");
    args.push(fs.join(tmpDataDir,"log"));
    if (protocol === "ssl") {
      args.push("--server.keyfile");
      args.push(fs.join("UnitTests","server.pem"));
    }
    instanceInfo.pid = executeExternal(ArangoServerState.executablePath(), 
                                       args);
  }

  // Wait until the server/coordinator is up:
  var up = false;
  var url;
  if (protocol === "ssl") {
    url = "https";
  }
  else {
    url = "http";
  }
  pos = endpoint.indexOf("://");
  url += endpoint.substr(pos);
  while (true) {
    wait(0.5);
    var r = download(url+"/_api/version","",makeAuthorisationHeaders(options));
    if (!r.error && r.code === 200) {
      break;
    }
  }

  instanceInfo.endpoint = endpoint;
  instanceInfo.url = url;

  return instanceInfo;
}

function shutdownInstance (instanceInfo, options) {
  if (options.cluster) {
    instanceInfo.kickstarter.shutdown();
    instanceInfo.kickstarter.cleanup();
  }
  else {
    download(instanceInfo.url+"/_admin/shutdown","",
             makeAuthorisationHeaders(options));
    wait(10);
    killExternal(instanceInfo.pid);
  }
  fs.removeDirectoryRecursive(instanceInfo.tmpDataDir);
}

function makePath (path) {
  return fs.join.apply(null,path.split("/"));
}

var tests_shell_common =
  [ makePath("js/common/tests/shell-require.js"),
    makePath("js/common/tests/shell-aqlfunctions.js"),
    makePath("js/common/tests/shell-attributes.js"),
    makePath("js/common/tests/shell-base64.js"),
    makePath("js/common/tests/shell-collection.js"),
    makePath("js/common/tests/shell-collection-volatile.js"),
    makePath("js/common/tests/shell-crypto.js"),
    makePath("js/common/tests/shell-database.js"),
    makePath("js/common/tests/shell-document.js"),
    makePath("js/common/tests/shell-download.js"),
    makePath("js/common/tests/shell-edge.js"),
    makePath("js/common/tests/shell-fs.js"),
    makePath("js/common/tests/shell-graph-traversal.js"),
    makePath("js/common/tests/shell-graph-algorithms.js"),
    makePath("js/common/tests/shell-graph-measurement.js"),
    makePath("js/common/tests/shell-keygen.js"),
    makePath("js/common/tests/shell-simple-query.js"),
    makePath("js/common/tests/shell-statement.js"),
    makePath("js/common/tests/shell-transactions.js"),
    makePath("js/common/tests/shell-unload.js"),
    makePath("js/common/tests/shell-users.js"),
    makePath("js/common/tests/shell-index.js"),
    makePath("js/common/tests/shell-index-geo.js"),
    makePath("js/common/tests/shell-cap-constraint.js"),
    makePath("js/common/tests/shell-unique-constraint.js"),
    makePath("js/common/tests/shell-hash-index.js"),
    makePath("js/common/tests/shell-fulltext.js"),
    makePath("js/common/tests/shell-graph.js")
  ];

var tests_shell_server_only =
  [
    makePath("js/server/tests/cluster.js"),
    makePath("js/server/tests/compaction.js"),
    makePath("js/server/tests/transactions.js"),
    makePath("js/server/tests/routing.js"),
    makePath("js/server/tests/shell-any.js"),
    makePath("js/server/tests/shell-bitarray-index.js"),
    makePath("js/server/tests/shell-database.js"),
    makePath("js/server/tests/shell-foxx.js"),
    makePath("js/server/tests/shell-foxx-repository.js"),
    makePath("js/server/tests/shell-foxx-model.js"),
    makePath("js/server/tests/shell-foxx-base-middleware.js"),
    makePath("js/server/tests/shell-foxx-template-middleware.js"),
    makePath("js/server/tests/shell-foxx-format-middleware.js"),
    makePath("js/server/tests/shell-foxx-preprocessor.js"),
    makePath("js/server/tests/shell-skiplist-index.js"),
    makePath("js/server/tests/shell-skiplist-rm-performance.js"),
    makePath("js/server/tests/shell-skiplist-correctness.js")
  ];

var tests_shell_server = tests_shell_common.concat(tests_shell_server_only);

var tests_shell_server_ahuacatl =
  [
    makePath("js/server/tests/ahuacatl-ranges.js"),
    makePath("js/server/tests/ahuacatl-queries-optimiser.js"),
    makePath("js/server/tests/ahuacatl-queries-optimiser-in.js"),
    makePath("js/server/tests/ahuacatl-queries-optimiser-limit.js"),
    makePath("js/server/tests/ahuacatl-queries-optimiser-sort.js"),
    makePath("js/server/tests/ahuacatl-queries-optimiser-ref.js"),
    makePath("js/server/tests/ahuacatl-escaping.js"),
    makePath("js/server/tests/ahuacatl-functions.js"),
    makePath("js/server/tests/ahuacatl-variables.js"),
    makePath("js/server/tests/ahuacatl-bind.js"),
    makePath("js/server/tests/ahuacatl-complex.js"),
    makePath("js/server/tests/ahuacatl-logical.js"),
    makePath("js/server/tests/ahuacatl-arithmetic.js"),
    makePath("js/server/tests/ahuacatl-relational.js"),
    makePath("js/server/tests/ahuacatl-ternary.js"),
    makePath("js/server/tests/ahuacatl-parse.js"),
    makePath("js/server/tests/ahuacatl-hash.js"),
    makePath("js/server/tests/ahuacatl-skiplist.js"),
    makePath("js/server/tests/ahuacatl-cross.js"),
    makePath("js/server/tests/ahuacatl-graph.js"),
    makePath("js/server/tests/ahuacatl-edges.js"),
    makePath("js/server/tests/ahuacatl-refaccess-variable.js"),
    makePath("js/server/tests/ahuacatl-refaccess-attribute.js"),
    makePath("js/server/tests/ahuacatl-queries-simple.js"),
    makePath("js/server/tests/ahuacatl-queries-variables.js"),
    makePath("js/server/tests/ahuacatl-queries-geo.js"),
    makePath("js/server/tests/ahuacatl-queries-fulltext.js"),
    makePath("js/server/tests/ahuacatl-queries-collection.js"),
    makePath("js/server/tests/ahuacatl-queries-noncollection.js"),
    makePath("js/server/tests/ahuacatl-subquery.js"),
    makePath("js/server/tests/ahuacatl-operators.js")
  ];

var tests_shell_server_ahuacatl_extended =
  [
    makePath("js/server/tests/ahuacatl-ranges-combined-1.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-2.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-3.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-4.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-5.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-6.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-7.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-8.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-9.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-10.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-11.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-12.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-13.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-14.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-15.js"),
    makePath("js/server/tests/ahuacatl-ranges-combined-16.js")
  ];

var tests_shell_client_only = 
  [
    makePath("js/client/tests/shell-endpoints.js"),
    makePath("js/client/tests/shell-fm.js"),
    makePath("js/client/tests/client.js")
  ];

var tests_shell_client = tests_shell_common.concat(tests_shell_client_only);

function runThere (options, instanceInfo, file) {
  var r;
  try {
    var t = 'var runTest = require("jsunity").runTest; '+
            'return runTest("'+file+'");';
    var o = makeAuthorisationHeaders(options);
    o.method = "POST";
    o.timeout = 24*3600;
    r = download(instanceInfo.url+"/_admin/execute?returnAsJSON=true",t,o);
    if (!r.error && r.code === 200) {
      r = JSON.parse(r.body);
    }
  }
  catch (err) {
    r = err;
  }
  return r;
}

function executeAndWait (cmd, args) {
  var pid = executeExternal(cmd, args);
  return statusExternal(pid, true).exit;
}

function runInArangosh (options, instanceInfo, file, addArgs) {
  var args = makeTestingArgsClient(options);
  var topDir = findTopDir();
  args.push("--server.endpoint");
  args.push(instanceInfo.endpoint);
  args.push("--javascript.unit-tests");
  args.push(fs.join(topDir,file));
  if (addArgs !== undefined) {
    args = args.concat(addArgs);
  }
  var arangosh = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                      "..","arangosh"));
  return executeAndWait(arangosh, args);
}

function runArangoshCmd (options, instanceInfo, cmds) {
  var args = makeTestingArgsClient(options);
  var topDir = findTopDir();
  args.push("--server.endpoint");
  args.push(instanceInfo.endpoint);
  args = args.concat(cmds);
  var arangosh = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                      "..","arangosh"));
  return executeAndWait(arangosh, args);
}

function performTests(options, testList) {
  var instanceInfo = startInstance("tcp",options);
  var results = {};
  var i;
  var te;
  for (i = 0; i < testList.length; i++) {
    te = testList[i];
    print("\nTrying",te,"...");
    var r = runThere(options, instanceInfo, te);
    results[te] = r;
    if (r !== true && !options.force) {
      break;
    }
  }
  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return results;
}

testFuncs.shell_server = function (options) {
  return performTests(options, tests_shell_server);
};

testFuncs.shell_server_only = function (options) {
  return performTests(options, tests_shell_server_only);
};

testFuncs.shell_server_ahuacatl = function(options) {
  if (!options.skipAhuacatl) {
    if (options.skipRanges) {
      return performTests(options, tests_shell_server_ahuacatl);
    }
    return performTests(options, tests_shell_server_ahuacatl.concat(
                                 tests_shell_server_ahuacatl_extended));
  }
  return "skipped";
};

testFuncs.shell_client = function(options) {
  var topDir = findTopDir();
  var instanceInfo = startInstance("tcp",options);
  var results = {};
  var i;
  var te;
  for (i = 0; i < tests_shell_client.length; i++) {
    te = tests_shell_client[i];
    print("\nTrying",te,"...");
    var r = runInArangosh(options, instanceInfo, te);
    results[te] = r;
    if (r !== 0 && !options.force) {
      break;
    }
  }
  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return results;
};

testFuncs.config = function (options) {
  var topDir = findTopDir();
  var results = {};
  var ts = ["arangod", "arangob", "arangodump", "arangoimp", "arangorestore",
            "arangosh"];
  var t;
  var i;
  for (i = 0; i < ts.length; i++) {
    t = ts[i];
    results[t] = executeAndWait(fs.join(topDir,"bin",t),
        ["--configuration", fs.join(topDir,"etc","arangodb",t+".conf"),
         "--help"]);
    print("Config test "+t+"...",results[t]);
  }
  for (i = 0; i < ts.length; i++) {
    t = ts[i];
    results[t+"_rel"] = executeAndWait(fs.join(topDir,"bin",t),
        ["--configuration", fs.join(topDir,"etc","relative",
                                    t+".conf"), "--help"]);
    print("Config test "+t+" (relative)...",results[t+"_rel"]);
  }

  return results;
};

testFuncs.boost = function (options) {
  var topDir = findTopDir();
  var results = {};
  if (!options.skipBoost) {
    results.basics = executeAndWait(fs.join(topDir,"UnitTests","basics_suite"),
                                    ["--show_progress"]);
  }
  if (!options.skipGeo) {
    results.geo_suite = executeAndWait(
                          fs.join(topDir,"UnitTests","geo_suite"),
                          ["--show_progress"]);
  }
  return results;
};

testFuncs.single = function (options) {
  var instanceInfo = startInstance("tcp",options);
  var result = { };
  var r;
  if (options.test !== undefined) {
    var te = options.test;
    result.test = te;
    if (!options.skipServer) {
      print("\nTrying",te,"on server...");
      result.server = runThere(options, instanceInfo, makePath(te));
    }
    if (!options.skipClient) {
      var topDir = findTopDir();
      var args = makeTestingArgsClient(options);
      args.push("--server.endpoint");
      args.push(instanceInfo.endpoint);
      args.push("--javascript.unit-tests");
      args.push(fs.join(topDir,te));
      print("\nTrying",te,"on client...");
      var arangosh = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                          "..","arangosh"));
      result.client = executeAndWait(arangosh, args);
    }
  }
  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return result;
};

function rubyTests (options, ssl) {
  var instanceInfo;
  if (ssl) {
    instanceInfo = startInstance("ssl",options);
  }
  else {
    instanceInfo = startInstance("tcp",options);
  }

  var tmpname = fs.getTempFile()+".rb";
  fs.write(tmpname,'RSpec.configure do |c|\n'+
                   '  c.add_setting :ARANGO_SERVER\n'+
                   '  c.ARANGO_SERVER = "' + 
                          instanceInfo.endpoint.substr(6) + '"\n'+
                   '  c.add_setting :ARANGO_SSL\n'+
                   '  c.ARANGO_SSL = "' + (ssl ? '1' : '0') + '"\n'+
                   '  c.add_setting :ARANGO_USER\n'+
                   '  c.ARANGO_USER = "' + options.username + '"\n'+
                   '  c.add_setting :ARANGO_PASSWORD\n'+
                   '  c.ARANGO_PASSWORD = "' + options.password + '"\n'+
                   'end\n');
  var files = fs.list(fs.join("UnitTests","HttpInterface"));
  var result = {};
  var args;
  var i;
  for (i = 0; i < files.length; i++) {
    var n = files[i];
    if (n.substr(0,4) === "api-" && n.substr(-3) === ".rb") {
      print("Considering",n,"...");
      if ((n.indexOf("-cluster-") === -1 || options.cluster) &&
          (n.indexOf("-noncluster-") === -1 || options.cluster === false) &&
          n.indexOf("replication") === -1) {
        args = ["--color", "-I", fs.join("UnitTests","HttpInterface"),
                "--format", "d", "--require", tmpname,
                fs.join("UnitTests","HttpInterface",n)];
        var pid = executeExternal("rspec", args);
        var r = statusExternal(pid, true);
        result[n] = r.exit;
        if (r.exit !== 0 && !options.force) {
          break;
        }
      }
      else {
        print("Skipped because of cluster/non-cluster or replication.");
      }
    }
  }
  
  print("Shutting down...");
  fs.remove(tmpname);
  shutdownInstance(instanceInfo,options);
  print("done.");
  return result;
}

testFuncs.http_server = function (options) {
  return rubyTests(options, false);
};

testFuncs.ssl_server = function (options) {
  return rubyTests(options, true);
};

function runArangoImp (options, instanceInfo, what) {
  var topDir = findTopDir();
  var args = ["--server.username",             options.username,
              "--server.password",             options.password,
              "--server.endpoint",             instanceInfo.endpoint,
              "--file",                        fs.join(topDir,what.data),
              "--collection",                  what.coll,
              "--type",                        what.type];
  if (what.create !== undefined) {
    args.push("--create-collection");
    args.push(what.create);
  }
  var arangoimp = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                       "..","arangoimp"));
  return executeAndWait(arangoimp, args);
}

function runArangoDumpRestore (options, instanceInfo, which, database) {
  var topDir = findTopDir();
  var args = ["--configuration",               "none",
              "--server.username",             options.username,
              "--server.password",             options.password,
              "--server.endpoint",             instanceInfo.endpoint,
              "--server.database",             database];
  var exe;
  if (which === "dump") {
    args.push("--output-directory");
    args.push(fs.join(instanceInfo.tmpDataDir,"dump"));
    exe = fs.normalize(fs.join(ArangoServerState.executablePath(),
                               "..","arangodump"));
  }
  else {
    args.push("--input-directory");
    args.push(fs.join(instanceInfo.tmpDataDir,"dump"));
    exe = fs.normalize(fs.join(ArangoServerState.executablePath(),
                               "..","arangorestore"));
  }
  return executeAndWait(exe, args);
}

function runArangoBenchmark (options, instanceInfo, cmds) {
  var topDir = findTopDir();
  var args = ["--configuration",               "none",
              "--quiet",
              "--server.username",             options.username,
              "--server.password",             options.password,
              "--server.endpoint",             instanceInfo.endpoint];
  args = args.concat(cmds);
  var exe = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                 "..","arangob"));
  return executeAndWait(exe, args);
}

var impTodo = [ 
  {id: "json1", data: makePath("UnitTests/import-1.json"),
   coll: "UnitTestsImportJson1", type: "json", create: undefined},
  {id: "json2", data: makePath("UnitTests/import-2.json"),
   coll: "UnitTestsImportJson2", type: "json", create: undefined},
  {id: "json3", data: makePath("UnitTests/import-3.json"),
   coll: "UnitTestsImportJson3", type: "json", create: undefined},
  {id: "json4", data: makePath("UnitTests/import-4.json"),
   coll: "UnitTestsImportJson4", type: "json", create: undefined},
  {id: "csv1", data: makePath("UnitTests/import-1.csv"),
   coll: "UnitTestsImportCsv1", type: "csv", create: "true"},
  {id: "csv2", data: makePath("UnitTests/import-2.csv"),
   coll: "UnitTestsImportCsv2", type: "csv", create: "true"},
  {id: "tsv1", data: makePath("UnitTests/import-1.tsv"),
   coll: "UnitTestsImportTsv1", type: "tsv", create: "true"},
  {id: "tsv2", data: makePath("UnitTests/import-2.tsv"),
   coll: "UnitTestsImportTsv2", type: "tsv", create: "true"},
  {id: "edge", data: makePath("UnitTests/import-edges.json"),
   coll: "UnitTestsImportEdge", type: "json", create: "false"}
];

testFuncs.importing = function (options) {
  var instanceInfo = startInstance("tcp",options);

  var result = {};
  try {
    var r = runInArangosh(options, instanceInfo, 
                          makePath("js/server/tests/import-setup.js"));
    result.setup = r;
    if (r !== 0) {
      throw "banana";
    }
    var i;
    for (i = 0; i < impTodo.length; i++) {
      r = runArangoImp(options, instanceInfo, impTodo[i]);
      result[impTodo[i].id] = r;
      if (r !== 0 && !options.force) {
        throw "banana";
      }
    }
    r = runInArangosh(options, instanceInfo,
                      makePath("js/server/tests/import.js"));
    result.check = r;
    r = runInArangosh(options, instanceInfo,
                      makePath("js/server/tests/import-teardown.js"));
    result.teardown = r;
  }
  catch (banana) {
  }

  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return result;
};

testFuncs.upgrade = function (options) {
  if (options.cluster) {
    return true;
  }

  var result = {};

  var topDir = findTopDir();
  var tmpDataDir = fs.getTempFile();
  fs.makeDirectoryRecursive(tmpDataDir);

  // We use the PortFinder to find a free port for our subinstance,
  // to this end, we have to fake a dummy dispatcher:
  var dispatcher = {endpoint: "tcp://localhost:", avoidPorts: {}, id: "me"};
  var pf = new PortFinder([8529],dispatcher);
  var port = pf.next();
  var args = makeTestingArgs();
  args.push("--server.endpoint");
  var endpoint = "tcp://127.0.0.1:"+port;
  args.push(endpoint);
  args.push("--database.directory");
  args.push(fs.join(tmpDataDir,"data"));
  fs.makeDirectoryRecursive(fs.join(tmpDataDir,"data"));
  args.push("--upgrade");
  result.first = executeAndWait(ArangoServerState.executablePath(), args);

  if (result.first !== 0 && !options.force) {
    return result;
  }
  result.second = executeAndWait(ArangoServerState.executablePath(), args);

  fs.removeDirectoryRecursive(tmpDataDir);

  return result;
};

testFuncs.foxx_manager = function (options) {
  print("foxx_manager tests...");
  var instanceInfo = startInstance("tcp",options);
  var results = {};
  
  results.update = runArangoshCmd(options, instanceInfo, 
                                  ["--configuration",
                                   "etc/relative/foxx-manager.conf",
                                   "update"]);
  if (results.update === 0 || options.force) {
    results.search = runArangoshCmd(options, instanceInfo, 
                                    ["--configuration",
                                     "etc/relative/foxx-manager.conf",
                                     "search","itzpapalotl"]);
  }
  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return results;
};

testFuncs.dump = function (options) {
  print("dump tests...");
  var instanceInfo = startInstance("tcp",options);
  var results = {};
  results.setup = runInArangosh(options, instanceInfo, 
                                makePath("js/server/tests/dump-setup.js"));
  if (results.setup === 0) {
    results.dump = runArangoDumpRestore(options, instanceInfo, "dump",
                                        "UnitTestsDumpSrc");
    results.restore = runArangoDumpRestore(options, instanceInfo, "restore",
                                           "UnitTestsDumpDst");
    results.test = runInArangosh(options, instanceInfo,
                                 makePath("js/server/tests/dump.js"),
                                 [ "--server.database", "UnitTestsDumpDst" ]);
    results.tearDown = runInArangosh(options, instanceInfo,
                               makePath("js/server/tests/dump-teardown.js"));
  }
  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return results;
};

var benchTodo = [
  ["--requests","10000","--concurrency","2","--test","version", 
   "--async","true"],
  ["--requests","20000","--concurrency","1","--test","version", 
   "--async","true"],
  ["--requests","100000","--concurrency","2","--test","shapes", 
   "--batch-size","16"],
  ["--requests","1000","--concurrency","2","--test","version", 
   "--batch-size", "16"],
  ["--requests","100","--concurrency","1","--test","version", 
   "--batch-size", "0"]
];
  
testFuncs.arangob = function (options) {
  print("arangob tests...");
  var instanceInfo = startInstance("tcp",options);
  var results = {};
  var i,r;
  for (i = 0; i < benchTodo.length; i++) {
    r = runArangoBenchmark(options, instanceInfo, benchTodo[i]);
    results[i] = r;
    if (r !== 0 && !options.force) {
      break;
    }
  }

  print("Shutting down...");
  shutdownInstance(instanceInfo,options);
  print("done.");
  return results;
};

var allTests = 
  [
    "make",
    "config",
    "boost",
    "shell_server",
    "shell_server_ahuacatl",
    "http_server",
    "ssl_server",
    "shell_client",
    "dump",
    "arangob",
    "importing",
    "upgrade",
    // "dfdb",    needs input redirection, but is not a good test anyway
    "foxx_manager",
    "authentication",
    "authentication_parameters"
  ];

function printUsage () {
  print("Usage: UnitTest( which, options )");
  print('       where "which" is one of:');
  print('         "all": do all tests');
  var i;
  for (i in testFuncs) {
    if (testFuncs.hasOwnProperty(i)) {
      print('         "'+i+'"');
    }
  }
  print('       and options can contain the following boolean properties:');
  print('         "force": continue despite a failed test'); 
  print('         "skipBoost": skip the boost unittests');
  print('         "skipGeo": skip the geo index tests');
  print('         "skipAhuacatl": skip the ahuacatl tests');
  print('         "skipRanges": skip the ranges tests');
  print('         "valgrind": arangods are run with valgrind');
  print('         "cluster": tests are run on a small local cluster');
}

function UnitTest (which, options) {
  if (typeof options !== "object") {
    options = {};
  }
  _.defaults(options, optionsDefaults);
  if (which === undefined) {
    printUsage();
    return;
  }
  if (which === "all") {
    var n;
    var results = {};
    for (n = 0;n < allTests.length;n++) {
      results[allTests[n]] = testFuncs[allTests[n]](options);
    }
    return results;
  }
  if (!testFuncs.hasOwnProperty(which)) {
    printUsage();
    throw 'Unknown test "'+which+'"';
  }
  var r = {};
  r[which] = testFuncs[which](options);
  return r;
}

exports.UnitTest = UnitTest;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "\\(/// @brief\\|/// @addtogroup\\|// --SECTION--\\|/// @page\\|/// @\\}\\)"
// End:
