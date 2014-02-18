/*jslint indent: 2, nomen: true, maxlen: 120, sloppy: true, vars: true, white: true, plusplus: true, stupid: true */
/*global module, require, exports, ArangoServerState, SYS_TEST_PORT */

////////////////////////////////////////////////////////////////////////////////
/// @brief Cluster kickstarting functionality using dispatchers
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

// -----------------------------------------------------------------------------
// --SECTION--                                         Kickstarter functionality
// -----------------------------------------------------------------------------

var download = require("internal").download;
var executeExternal = require("internal").executeExternal;
var killExternal = require("internal").killExternal;
var statusExternal = require("internal").statusExternal;
var base64Encode = require("internal").base64Encode;

var fs = require("fs");
var wait = require("internal").wait;

var exchangePort = require("org/arangodb/cluster/planner").exchangePort;

var console = require("console");

var launchActions = {};
var shutdownActions = {};
var cleanupActions = {};
var isHealthyActions = {};

function getAddrPort (endpoint) {
  var pos = endpoint.indexOf("://");
  if (pos !== -1) {
    return endpoint.substr(pos+3);
  }
  return endpoint;
}

function getAddr (endpoint) {
  var addrPort = getAddrPort(endpoint);
  var pos = addrPort.indexOf(":");
  if (pos !== -1) {
    return addrPort.substr(0,pos);
  }
  return addrPort;
}

function getPort (endpoint) {
  var pos = endpoint.lastIndexOf(":");
  if (pos !== -1) {
    return parseInt(endpoint.substr(pos+1),10);
  }
  return 8529;
}

function encode (st) {
  var st2 = "";
  var i;
  for (i = 0; i < st.length; i++) {
    if (st[i] === "_") {
      st2 += "@U";
    }
    else if (st[i] === "@") {
      st2 += "@@";
    } 
    else {
      st2 += st[i];
    }
  }
  return encodeURIComponent(st2);
}

function sendToAgency (agencyURL, path, obj) {
  var res;
  var body;
  
  console.info("Sending %s to agency...", path);
  if (typeof obj === "string") {
    var count = 0;
    while (count++ <= 2) {
      body = "value="+encodeURIComponent(obj);
      res = download(agencyURL+path,body,
          {"method":"PUT", "followRedirects": true,
           "headers": { "Content-Type": "application/x-www-form-urlencoded"}});
      if (res.code === 201) {
        return true;
      }
    }
    return res;
  }
  if (typeof obj !== "object") {
    return "Strange object found: not a string or object";
  }
  var keys = Object.keys(obj);
  var i;
  if (keys.length !== 0) {
    for (i = 0; i < keys.length; i++) {
      res = sendToAgency (agencyURL, path+"/"+encode(keys[i]), obj[keys[i]]);
      if (res !== true) {
        return res;
      }
    }
    return true;
  }
  // Create a directory
  var count2 = 0;
  while (count2++ <= 2) {
    body = "dir=true";
    res = download(agencyURL+path,body,
          {"method": "PUT", "followRedirects": true,
           "headers": { "Content-Type": "application/x-www-form-urlencoded"}});
    if (res.code === 201) {
      return true;
    }
  }
  return res;
}

launchActions.startAgent = function (dispatchers, cmd, isRelaunch) {

  console.info("Starting agent...");

  // First find out our own data directory:
  var myDataDir = fs.normalize(fs.join(ArangoServerState.basePath(),".."));
  var dataPath = fs.makeAbsolute(cmd.dataPath);
  if (dataPath !== cmd.dataPath) {   // path was relative
    dataPath = fs.normalize(fs.join(myDataDir,cmd.dataPath));
  }

  var agentDataDir = fs.join(dataPath, "agent"+cmd.agencyPrefix+cmd.extPort);
  if (!isRelaunch) {
    if (fs.exists(agentDataDir)) {
      fs.removeDirectoryRecursive(agentDataDir,true);
    }
  }
  var extEndpoint = getAddrPort(
                          exchangePort(dispatchers[cmd.dispatcher].endpoint,
                                       cmd.extPort));
  var args = ["-data-dir", agentDataDir,
              "-name", "agent"+cmd.agencyPrefix+cmd.extPort,
              "-bind-addr", (cmd.onlyLocalhost ? "127.0.0.1:" 
                                               : "0.0.0.0:")+cmd.extPort,
              "-addr", extEndpoint,
              "-peer-bind-addr", (cmd.onlyLocalhost ? "127.0.0.1:"
                                                    : "0.0.0.0:")+cmd.intPort,
              "-peer-addr", getAddrPort(
                          exchangePort(dispatchers[cmd.dispatcher].endpoint,
                                       cmd.intPort)) ];
  var i;
  if (cmd.peers.length > 0) {
    args.push("-peers");
    var st = getAddrPort(cmd.peers[0]);
    for (i = 1; i < cmd.peers.length; i++) {
      st = st + "," + getAddrPort(cmd.peers[i]);
    }
    args.push(getAddrPort(cmd.peers[0]));
  }
  var agentPath = fs.makeAbsolute(cmd.agentPath);
  if (agentPath !== cmd.agentPath) {
    if (cmd.agentPath === "") {
      agentPath = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                       "..", "etcd" ));
    }
    else {
      agentPath = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                       "..", cmd.agentPath ));
    }
  }
  var pid = executeExternal(agentPath, args);
  var res;
  var count = 0;
  while (++count < 20) {
    wait(0.5);   // Wait a bit to give it time to startup
    res = download("http://localhost:"+cmd.extPort+"/v2/keys/");
    if (res.code === 200) {
      return {"error":false, "isStartAgent": true, "pid": pid, 
              "endpoint": "tcp://"+extEndpoint};
    }
  }
  return {"error":true, "isStartAgent": true, 
          "errorMessage": "agency did not come alive"};
};

launchActions.sendConfiguration = function (dispatchers, cmd, isRelaunch) {
  if (isRelaunch) {
    // nothing to do here
    console.info("Waiting 10 seconds for agency to come alive...");
    wait(10);
    return {"error":false, "isSendConfiguration": true};
  }
  var url = "http://"+getAddrPort(cmd.agency.endpoints[0])+"/v2/keys";
  var res = sendToAgency(url, "", cmd.data);
  if (res === true) {
    return {"error":false, "isSendConfiguration": true};
  }
  return {"error":true, "isSendConfiguration": true, "suberror": res};
};

launchActions.startServers = function (dispatchers, cmd, isRelaunch) {

  // First find out our own data directory to setup base for relative paths:
  var myDataDir = fs.normalize(fs.join(ArangoServerState.basePath(),".."));
  var dataPath = fs.makeAbsolute(cmd.dataPath);
  if (dataPath !== cmd.dataPath) {   // path was relative
    dataPath = fs.normalize(fs.join(myDataDir, cmd.dataPath));
  }
  var logPath = fs.makeAbsolute(cmd.logPath);
  if (logPath !== cmd.logPath) {    // path was relative
    logPath = fs.normalize(fs.join(myDataDir, cmd.logPath));
  }

  var url = "http://"+getAddrPort(cmd.agency.endpoints[0])+"/v2/keys/"+
            cmd.agency.agencyPrefix+"/";
  console.info("Downloading %sLaunchers/%s", url, cmd.name);
  var res = download(url+"Launchers/"+cmd.name,"",{method:"GET",
                                                   followRedirects:true});
  if (res.code !== 200) {
    return {"error": true, "isStartServers": true, "suberror": res};
  }
  var body = JSON.parse( res.body );
  var info = JSON.parse(body.node.value);
  var id,ep,args,pids,port,endpoints,roles;

  console.info("Starting servers...");
  var i;
  var servers = info.DBservers.concat(info.Coordinators);
  roles = [];
  for (i = 0; i < info.DBservers.length; i++) {
    roles.push("DBserver");
  }
  for (i = 0; i < info.Coordinators.length; i++) {
    roles.push("Coordinator");
  }
  pids = [];
  endpoints = [];
  for (i = 0; i < servers.length; i++) {
    id = servers[i];
    console.info("Downloading %sTarget/MapIDToEndpoint/%s", url, id);
    res = download(url+"Target/MapIDToEndpoint/"+id);
    if (res.code !== 200) {
      return {"error": true, "pids": pids,
              "isStartServers": true, "suberror": res};
    }
    console.info("Starting server %s",id);
    body = JSON.parse(res.body);
    ep = JSON.parse(body.node.value);
    port = getPort(ep);
    args = ["--cluster.my-id", id, 
            "--cluster.agency-prefix", cmd.agency.agencyPrefix,
            "--cluster.agency-endpoint", cmd.agency.endpoints[0],
            "--server.endpoint"];
    if (cmd.onlyLocalhost) {
      args.push("tcp://127.0.0.1:"+port);
    }
    else {
      args.push("tcp://0.0.0.0:"+port);
    }
    args.push("--log.file");
    var logfile = fs.join(logPath,"log-"+cmd.agency.agencyPrefix+"-"+id);
    args.push(logfile);
    if (!isRelaunch) {
      if (fs.exists(logfile)) {
        fs.remove(logfile);
      }
    }
    var datadir = fs.join(dataPath,"data-"+cmd.agency.agencyPrefix+"-"+id);
    args.push("--database.directory");
    args.push(datadir);
    if (!isRelaunch) {
      if (fs.exists(datadir)) {
        fs.removeDirectoryRecursive(datadir,true);
      }
      fs.makeDirectory(datadir);
    }
    args = args.concat(dispatchers[cmd.dispatcher].arangodExtraArgs);
    var arangodPath = fs.makeAbsolute(cmd.arangodPath);
    if (arangodPath !== cmd.arangodPath) {
      if (cmd.arangodPath === "") {
        arangodPath = ArangoServerState.executablePath();
      }
      else {
        arangodPath = fs.normalize(fs.join(ArangoServerState.executablePath(),
                                           "..", cmd.arangodPath ));
      }
    }
    pids.push(executeExternal(arangodPath, args));
    endpoints.push(exchangePort(dispatchers[cmd.dispatcher].endpoint,port));
  }
  return {"error": false, "isStartServers": true, 
          "pids": pids, "endpoints": endpoints, "roles": roles};
};

launchActions.createSysColls = function (dispatchers, cmd) {
  var url = cmd.url + "/_api/collection";
  var o = { "method": "POST" };
  var collinfo = { "name": "_aal", "isSystem": true, "numberOfShards": 1 };
  download(url+"/_api/collection", JSON.stringify(collinfo), o);
  collinfo.name = "...";
  download(url+"/_api/collection", JSON.stringify(collinfo), o);
  collinfo.name = "...";
  download(url+"/_api/collection", JSON.stringify(collinfo), o);
};

shutdownActions.startAgent = function (dispatchers, cmd, run) {
  console.info("Shutting down agent %s", run.pid);
  killExternal(run.pid);
  return {"error": false, "isStartAgent": true};
};

shutdownActions.sendConfiguration = function (dispatchers, cmd, run) {
  console.info("Waiting for 10 seconds for servers before shutting down agency.");
  wait(10);
  return {"error": false, "isSendConfiguration": true};
};

shutdownActions.startServers = function (dispatchers, cmd, run) {
  var i;
  var url;
  for (i = 0;i < run.endpoints.length;i++) {
    console.info("Using API to shutdown %s", run.pids[i].toString());
    url = "http://"+run.endpoints[i].substr(6)+"/_admin/shutdown";
    download(url);
  }
  console.info("Waiting 10 seconds for servers to shutdown gracefully...");
  wait(10);
  for (i = 0;i < run.pids.length;i++) {
    console.info("Shutting down %s the hard way...", run.pids[i].toString());
    killExternal(run.pids[i]);
  }
  return {"error": false, "isStartServers": true};
};

cleanupActions.startAgent = function (dispatchers, cmd) {

  console.info("Cleaning up agent...");

  // First find out our own data directory:
  var myDataDir = fs.normalize(fs.join(ArangoServerState.basePath(),".."));
  var dataPath = fs.makeAbsolute(cmd.dataPath);
  if (dataPath !== cmd.dataPath) {   // path was relative
    dataPath = fs.normalize(fs.join(myDataDir,cmd.dataPath));
  }

  var agentDataDir = fs.join(dataPath, "agent"+cmd.agencyPrefix+cmd.extPort);
  if (fs.exists(agentDataDir)) {
    fs.removeDirectoryRecursive(agentDataDir,true);
  }
  return {"error":false, "isStartAgent": true};
};

cleanupActions.sendConfiguration = function (dispatchers, cmd) {
  // nothing to do here
  return {"error":false, "isSendConfiguration": true};
};

cleanupActions.startServers = function (dispatchers, cmd, isRelaunch) {

  console.info("Cleaning up DBservers...");

  // First find out our own data directory to setup base for relative paths:
  var myDataDir = fs.normalize(fs.join(ArangoServerState.basePath(),".."));
  var dataPath = fs.makeAbsolute(cmd.dataPath);
  if (dataPath !== cmd.dataPath) {   // path was relative
    dataPath = fs.normalize(fs.join(myDataDir, cmd.dataPath));
  }
  var logPath = fs.makeAbsolute(cmd.logPath);
  if (logPath !== cmd.logPath) {    // path was relative
    logPath = fs.normalize(fs.join(myDataDir, cmd.logPath));
  }

  var servers = cmd.DBservers.concat(cmd.Coordinators);
  var i;
  for (i = 0; i < servers.length; i++) {
    var id = servers[i];
    var logfile = fs.join(logPath,"log-"+cmd.agency.agencyPrefix+"-"+id);
    if (fs.exists(logfile)) {
      fs.remove(logfile);
    }
    var datadir = fs.join(dataPath,"data-"+cmd.agency.agencyPrefix+"-"+id);
    if (fs.exists(datadir)) {
      fs.removeDirectoryRecursive(datadir,true);
    }
  }
  return {"error": false, "isStartServers": true};
};

isHealthyActions.startAgent = function (dispatchers, cmd, run) {
  console.info("Checking health of agent %s", run.pid);
  var r = statusExternal(run.pid);
  r.isStartAgent = true;
  r.error = false;
  return r;
};

isHealthyActions.sendConfiguration = function (dispatchers, cmd, run) {
  return {"error": false, "isSendConfiguration": true};
};

isHealthyActions.startServers = function (dispatchers, cmd, run) {
  var i;
  var r = [];
  for (i = 0;i < run.pids.length;i++) {
    console.info("Checking health of server %s", run.pids[i].toString());
    r.push(statusExternal(run.pids[i]));
  }
  return {"error": false, "isStartServers": true, "status": r};
};

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Cluster_Kickstarter_Constructor
/// @brief the cluster kickstarter constructor
///
/// @FUN{new require("org/arangodb/cluster/kickstarter").Kickstarter(@FA{plan})}
///
/// This constructor constructs a kickstarter object. Its first
/// argument is a cluster plan as for example provided by the planner
/// (see @ref JSF_Cluster_Planner_Constructor and the general
/// explanations before this reference). The second argument is 
/// optional and is taken to be "me" if omitted, it is the ID of the
/// dispatcher this object should consider itself to be. If the plan
/// contains startup commands for the dispatcher with this ID, these
/// commands are executed immediately. Otherwise they are handed over
/// to another responsible dispatcher via a REST call.
///
/// The resulting object of this constructors allows to launch,
/// shutdown, relaunch the cluster described in the plan.
////////////////////////////////////////////////////////////////////////////////

function Kickstarter (clusterPlan, myname) {
  this.clusterPlan = clusterPlan;
  if (myname === undefined) {
    this.myname = "me";
  }
  else {
    this.myname = myname;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Kickstarter_prototype_launch
/// @brief starts a cluster
///
/// @FUN{@FA{Kickstarter}.launch()}
///
/// This starts up a cluster as described in the plan which was given to
/// the constructor. To this end, other dispatchers are contacted as 
/// necessary. All startup commands for the local dispatcher are
/// executed immediately.
///
/// The result is an object that contains information about the started
/// processes, this object is also stored in the Kickstarter object
/// itself. We do not go into details here about the data structure,
/// but the most important information are the process IDs of the 
/// started processes. The corresponding shutdown method (see @ref 
/// JSF_Kickstarter_prototype_shutdown) needs this information to
/// shut down all processes.
///
/// Note that all data in the DBservers and all log files and all agency
/// information in the cluster is deleted by this call. This is because
/// it is intended to set up a cluster for the first time. See
/// the relaunch method (see @ref JSF_Kickstarter_prototype_relaunch)
/// for restarting a cluster without data loss.
////////////////////////////////////////////////////////////////////////////////

Kickstarter.prototype.launch = function () {
  var clusterPlan = this.clusterPlan;
  var myname = this.myname;
  var dispatchers = clusterPlan.dispatchers;
  var cmds = clusterPlan.commands;
  var results = [];
  var cmd;

  var error = false;
  var i;
  var res;
  for (i = 0; i < cmds.length; i++) {
    cmd = cmds[i];
    if (cmd.dispatcher === undefined || cmd.dispatcher === myname) {
      res = launchActions[cmd.action](dispatchers, cmd, false);
      results.push(res);
      if (res.error === true) {
        error = true;
        break;
      }
    }
    else {
      var ep = dispatchers[cmd.dispatcher].endpoint;
      var body = JSON.stringify({ "action": "launch",
                                  "clusterPlan": {
                                        "dispatchers": dispatchers,
                                        "commands": [cmd] },
                                  "myname": cmd.dispatcher });
      var url = "http" + ep.substr(3) + "/_admin/clusterDispatch";
      var hdrs = {};
      if (dispatchers[cmd.dispatcher].username !== undefined &&
          dispatchers[cmd.dispatcher].passwd !== undefined) {
        hdrs.Authorization = "Basic "+
                        base64Encode(dispatchers[cmd.dispatcher].username+":"+
                                     dispatchers[cmd.dispatcher].passwd);
      }
      var response = download(url, body, {"method": "POST", "headers": hdrs});
      if (response.code !== 200) {
        error = true;
        results.push({"error":true, "errorMessage": "bad HTTP response code",
                      "response": response});
      }
      else {
        try {
          res = JSON.parse(response.body);
          results.push(res.runInfo[0]);
        }
        catch (err) {
          error = true;
          results.push({"error":true, "errorMessage": "exception in JSON.parse"});
        }
      }
      if (error) {
        break;
      }
    }
  }
  this.runInfo = results;
  if (error) {
    return {"error": true, "errorMessage": "some error during launch",
            "runInfo": results};
  }
  return {"error": false, "errorMessage": "none",
          "runInfo": results};
};

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Kickstarter_prototype_relaunch
/// @brief restarts a cluster without deleting its data
///
/// @FUN{@FA{Kickstarter}.relaunch()}
///
/// This starts up a cluster as described in the plan which was given to
/// the constructor. To this end, other dispatchers are contacted as 
/// necessary. All startup commands for the local dispatcher are
/// executed immediately.
///
/// The result is an object that contains information about the started
/// processes, this object is also stored in the Kickstarter object
/// itself. We do not go into details here about the data structure,
/// but the most important information are the process IDs of the 
/// started processes. The corresponding shutdown method (see @ref 
/// JSF_Kickstarter_prototype_shutdown) needs this information to
/// shut down all processes.
///
/// Note that this methods needs that all data in the DBservers and the
/// agency information in the cluster are already set up properly. See
/// the launch method (see @ref JSF_Kickstarter_prototype_launch) for
/// starting a cluster for the first time.
////////////////////////////////////////////////////////////////////////////////

Kickstarter.prototype.relaunch = function () {
  var clusterPlan = this.clusterPlan;
  var myname = this.myname;
  var dispatchers = clusterPlan.dispatchers;
  var cmds = clusterPlan.commands;
  var results = [];
  var cmd;

  var error = false;
  var i;
  var res;
  for (i = 0; i < cmds.length; i++) {
    cmd = cmds[i];
    if (cmd.dispatcher === undefined || cmd.dispatcher === myname) {
      res = launchActions[cmd.action](dispatchers, cmd, true);
      results.push(res);
      if (res.error === true) {
        error = true;
        break;
      }
    }
    else {
      var ep = dispatchers[cmd.dispatcher].endpoint;
      var body = JSON.stringify({ "action": "relaunch",
                                  "clusterPlan": {
                                        "dispatchers": dispatchers,
                                        "commands": [cmd] },
                                  "myname": cmd.dispatcher });
      var url = "http" + ep.substr(3) + "/_admin/clusterDispatch";
      var hdrs = {};
      if (dispatchers[cmd.dispatcher].username !== undefined &&
          dispatchers[cmd.dispatcher].passwd !== undefined) {
        hdrs.Authorization = "Basic "+
                        base64Encode(dispatchers[cmd.dispatcher].username+":"+
                                     dispatchers[cmd.dispatcher].passwd);
      }
      var response = download(url, body, {"method": "POST", "headers": hdrs});
      if (response.code !== 200) {
        error = true;
        results.push({"error":true, "errorMessage": "bad HTTP response code",
                      "response": response});
      }
      try {
        res = JSON.parse(response.body);
        results.push(res.runInfo[0]);
      }
      catch (err) {
        results.push({"error":true, "errorMessage": "exception in JSON.parse"});
        error = true;
        break;
      }
    }
  }
  this.runInfo = results;
  if (error) {
    return {"error": true, "errorMessage": "some error during launch",
            "runInfo": results};
  }
  return {"error": false, "errorMessage": "none",
          "runInfo": results};
};

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Kickstarter_prototype_shutdown
/// @brief starts a cluster
///
/// @FUN{@FA{Kickstarter}.shutdown()}
///
/// This shuts down a cluster as described in the plan which was given to
/// the constructor. To this end, other dispatchers are contacted as 
/// necessary. All processes in the cluster are gracefully shut down
/// in the right order.
////////////////////////////////////////////////////////////////////////////////

Kickstarter.prototype.shutdown = function() {
  var clusterPlan = this.clusterPlan;
  var myname = this.myname;
  var dispatchers = clusterPlan.dispatchers;
  var cmds = clusterPlan.commands;
  var runInfo = this.runInfo;
  var results = [];
  var cmd;

  var error = false;
  var i;
  var res;
  for (i = cmds.length-1; i >= 0; i--) {
    cmd = cmds[i];
    var run = runInfo[i];
    if (cmd.dispatcher === undefined || cmd.dispatcher === myname) {
      res = shutdownActions[cmd.action](dispatchers, cmd, run);
      if (res.error === true) {
        error = true;
      }
      results.push(res);
    }
    else {
      var ep = dispatchers[cmd.dispatcher].endpoint;
      var body = JSON.stringify({ "action": "shutdown",
                                  "clusterPlan": {
                                        "dispatchers": dispatchers,
                                        "commands": [cmd] },
                                  "runInfo": [run],
                                  "myname": cmd.dispatcher });
      var url = "http" + ep.substr(3) + "/_admin/clusterDispatch";
      var hdrs = {};
      if (dispatchers[cmd.dispatcher].username !== undefined &&
          dispatchers[cmd.dispatcher].passwd !== undefined) {
        hdrs.Authorization = "Basic "+
                        base64Encode(dispatchers[cmd.dispatcher].username+":"+
                                     dispatchers[cmd.dispatcher].passwd);
      }
      var response = download(url, body, {"method": "POST", "headers": hdrs});
      if (response.code !== 200) {
        error = true;
        results.push({"error":true, "errorMessage": "bad HTTP response code",
                      "response": response});
      }
      else {
        try {
          res = JSON.parse(response.body);
          results.push(res.results[0]);
        }
        catch (err) {
          results.push({"error":true, 
                        "errorMessage": "exception in JSON.parse"});
          error = true;
        }
      }
    }
  }
  results = results.reverse();
  if (error) {
    return {"error": true, "errorMessage": "some error during shutdown",
            "results": results};
  }
  return {"error": false, "errorMessage": "none", "results": results};
};

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Kickstarter_prototype_cleanup
/// @brief cleans up all the data of a cluster that has been shutdown
///
/// @FUN{@FA{Kickstarter}.cleanup()}
///
/// This cleans up all the data and logs of a previously shut down cluster.
/// To this end, other dispatchers are contacted as necessary. 
/// Use shutdown (see @ref JSF_Kickstarter_prototype_shutdown) first and
/// use with caution, since potentially a lot of data is being erased with
/// this call!
////////////////////////////////////////////////////////////////////////////////

Kickstarter.prototype.cleanup = function() {
  var clusterPlan = this.clusterPlan;
  var myname = this.myname;
  var dispatchers = clusterPlan.dispatchers;
  var cmds = clusterPlan.commands;
  var results = [];
  var cmd;

  var error = false;
  var i;
  var res;
  for (i = 0; i < cmds.length; i++) {
    cmd = cmds[i];
    if (cmd.dispatcher === undefined || cmd.dispatcher === myname) {
      res = cleanupActions[cmd.action](dispatchers, cmd);
      results.push(res);
      if (res.error === true) {
        error = true;
      }
    }
    else {
      var ep = dispatchers[cmd.dispatcher].endpoint;
      var body = JSON.stringify({ "action": "cleanup",
                                  "clusterPlan": {
                                        "dispatchers": dispatchers,
                                        "commands": [cmd] },
                                  "myname": cmd.dispatcher });
      var url = "http" + ep.substr(3) + "/_admin/clusterDispatch";
      var hdrs = {};
      if (dispatchers[cmd.dispatcher].username !== undefined &&
          dispatchers[cmd.dispatcher].passwd !== undefined) {
        hdrs.Authorization = "Basic "+
                        base64Encode(dispatchers[cmd.dispatcher].username+":"+
                                     dispatchers[cmd.dispatcher].passwd);
      }
      var response = download(url, body, {"method": "POST", "headers": hdrs});
      if (response.code !== 200) {
        error = true;
        results.push({"error":true, "errorMessage": "bad HTTP response code",
                      "response": response});
      }
      else {
        try {
          res = JSON.parse(response.body);
          results.push(res.results[0]);
        }
        catch (err) {
          results.push({"error":true, 
                        "errorMessage": "exception in JSON.parse"});
          error = true;
        }
      }
    }
  }
  if (error) {
    return {"error": true, "errorMessage": "some error during cleanup",
            "results": results};
  }
  return {"error": false, "errorMessage": "none", "results": results};
};

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_Kickstarter_prototype_isHealthy
/// @brief checks that all processes of a running cluster are healthy
///
/// @FUN{@FA{Kickstarter}.isHealthy()}
///
/// This checks that all processes belonging to a running cluster are
/// healthy. To this end, other dispatchers are contacted as necessary.
/// At this stage it is only checked that the processes are still up and
/// running.
////////////////////////////////////////////////////////////////////////////////

Kickstarter.prototype.isHealthy = function() {
  var clusterPlan = this.clusterPlan;
  var myname = this.myname;
  var dispatchers = clusterPlan.dispatchers;
  var cmds = clusterPlan.commands;
  var runInfo = this.runInfo;
  var results = [];
  var cmd;

  var error = false;
  var i;
  var res;
  for (i = cmds.length-1; i >= 0; i--) {
    cmd = cmds[i];
    var run = runInfo[i];
    if (cmd.dispatcher === undefined || cmd.dispatcher === myname) {
      res = isHealthyActions[cmd.action](dispatchers, cmd, run);
      if (res.error === true) {
        error = true;
      }
      results.push(res);
    }
    else {
      var ep = dispatchers[cmd.dispatcher].endpoint;
      var body = JSON.stringify({ "action": "isHealthy",
                                  "clusterPlan": {
                                        "dispatchers": dispatchers,
                                        "commands": [cmd] },
                                  "runInfo": [run],
                                  "myname": cmd.dispatcher });
      var url = "http" + ep.substr(3) + "/_admin/clusterDispatch";
      var hdrs = {};
      if (dispatchers[cmd.dispatcher].username !== undefined &&
          dispatchers[cmd.dispatcher].passwd !== undefined) {
        hdrs.Authorization = "Basic "+
                        base64Encode(dispatchers[cmd.dispatcher].username+":"+
                                     dispatchers[cmd.dispatcher].passwd);
      }
      var response = download(url, body, {"method": "POST", "headers": hdrs});
      if (response.code !== 200) {
        error = true;
        results.push({"error":true, "errorMessage": "bad HTTP response code",
                      "response": response});
      }
      else {
        try {
          res = JSON.parse(response.body);
          results.push(res.results[0]);
        }
        catch (err) {
          results.push({"error":true, 
                        "errorMessage": "exception in JSON.parse"});
          error = true;
        }
      }
    }
  }
  results = results.reverse();
  if (error) {
    return {"error": true, "errorMessage": "some error during isHealthy check",
            "results": results};
  }
  return {"error": false, "errorMessage": "none", "results": results};
};

exports.Kickstarter = Kickstarter;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

/// Local Variables:
/// mode: outline-minor
/// outline-regexp: "/// @brief\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\|/\\*jslint"
/// End:
