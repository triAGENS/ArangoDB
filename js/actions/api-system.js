/*jslint indent: 2, nomen: true, maxlen: 100, sloppy: true, vars: true, white: true, plusplus: true, evil: true */
/*global require, exports, module, SYS_SHARDING_TEST */

////////////////////////////////////////////////////////////////////////////////
/// @brief administration actions
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var arangodb = require("org/arangodb");
var actions = require("org/arangodb/actions");

var db = arangodb.db;

var internal = require("internal");
var console = require("console");

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoAPI
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief routing function
////////////////////////////////////////////////////////////////////////////////

function routing (req, res) {
  var action;
  var execute;
  var next;
  var path = req.suffix.join("/");

  action = actions.firstRouting(req.requestType, req.suffix);

  execute = function () {
    if (action.route === undefined) {
      actions.resultNotFound(req, res, arangodb.ERROR_HTTP_NOT_FOUND, 
        "unknown path '" + path + "'");
      return;
    }

    if (action.route.path !== undefined) {
      req.path = action.route.path;
    }
    else {
      delete req.path;
    }

    if (action.prefix !== undefined) {
      req.prefix = action.prefix;
    }
    else {
      delete req.prefix;
    }

    if (action.suffix !== undefined) {
      req.suffix = action.suffix;
    }
    else {
      delete req.suffix;
    }

    if (action.urlParameters !== undefined) {
      req.urlParameters = action.urlParameters;
    }
    else {
      req.urlParameters = {};
    }

    var func = action.route.callback.controller;

    if (func === null || typeof func !== 'function') {
      func = actions.errorFunction(action.route,
                                   'Invalid callback definition found for route ' 
                                   + JSON.stringify(action.route));
    }

    try {
      func(req, res, action.route.callback.options, next);
    }
    catch (err) {
      var msg = 'A runtime error occurred while executing an action: '
                + String(err) + " " + String(err.stack);

      actions.errorFunction(action.route, msg)(req, res, action.route.callback.options, next);
    }
  };

  next = function () {
    action = actions.nextRouting(action);
    execute();
  };

  execute();
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoAPI
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief main routing action
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "",
  prefix : true,
  context : "admin",

  callback : routing
});

////////////////////////////////////////////////////////////////////////////////
/// @brief reloads the server authentication information
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/auth/reload",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    internal.reloadAuth();
    actions.resultOk(req, res, actions.HTTP_OK);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @brief reloads the AQL user functions
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/aql/reload",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    internal.reloadAqlFunctions();
    actions.resultOk(req, res, actions.HTTP_OK);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_routing_reloads
/// @brief reloads the routing information
///
/// @RESTHEADER{POST /_admin/routing/reload,reloads the routing collection}
///
/// @RESTDESCRIPTION
///
/// Reloads the routing information from the collection `routing`.
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Routing information was reloaded successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/routing/reload",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    internal.executeGlobalContextFunction("require(\"org/arangodb/actions\").reloadRouting()");
    console.warn("about to flush the routing cache");
    actions.resultOk(req, res, actions.HTTP_OK);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current routing information 
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/routing/routes",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    actions.resultOk(req, res, actions.HTTP_OK, actions.routingCache());
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_modules_flush
/// @brief flushes the modules cache
///
/// @RESTHEADER{POST /_admin/modules/flush,flushs the module cache}
///
/// @RESTDESCRIPTION
///
/// The call flushes the modules cache on the server. See `JSModulesCache`
/// for details about this cache.
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Module cache was flushed successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/modules/flush",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    internal.executeGlobalContextFunction("require(\"internal\").flushModuleCache()");
    console.warn("about to flush the modules cache");
    actions.resultOk(req, res, actions.HTTP_OK);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_time
/// @brief returns the system time
///
/// @RESTHEADER{GET /_admin/time,returns the system time}
///
/// @RESTDESCRIPTION
///
/// The call returns an object with the attribute `time`. This contains the
/// current system time as a Unix timestamp with microsecond precision.
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Time was returned successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/time",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    actions.resultOk(req, res, actions.HTTP_OK, { time : internal.time() });
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_sleep
/// @brief sleeps, this is useful for timeout tests
///
/// @RESTHEADER{GET /_admin/sleep?duration=5,sleeps for 5 seconds}
///
/// @RESTDESCRIPTION
///
/// The call returns an object with the attribute `duration`. This takes
/// as many seconds as the duration argument says.
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Sleep was conducted successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/sleep",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    var time = parseFloat(req.parameters.duration);
    if (isNaN(time)) {
      time = 3.0;
    }
    internal.wait(time);
    actions.resultOk(req, res, actions.HTTP_OK, { duration : time });
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_echo
/// @brief returns the request
///
/// @RESTHEADER{GET /_admin/echo,returns the current request}
///
/// @RESTDESCRIPTION
///
/// The call returns an object with the following attributes:
///
/// - `headers`: a list of HTTP headers received
///
/// - `requestType`: the HTTP request method (e.g. GET)
///
/// - `parameters`: list of URL parameters received
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Echo was returned successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/echo",
  context : "admin",
  prefix : true,

  callback : function (req, res) {
    res.responseCode = actions.HTTP_OK;
    res.contentType = "application/json; charset=utf-8";
    res.body = JSON.stringify(req);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_echo_database_name
/// @brief returns the database name
///
/// @RESTHEADER{GET /_admin/database_name,returns the database name}
///
/// @RESTDESCRIPTION
///
/// The call returns an object with the following attributes:
///
/// - `name`: the name of the database
///
/// @RESTRETURNCODES
///
/// @RESTRETURNCODE{200}
/// Name was returned successfully.
///
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/database-name",
  context : "admin",
  prefix : true,

  callback : function (req, res) {
    res.responseCode = actions.HTTP_OK;
    res.contentType = "application/json; charset=utf-8";
    res.body = JSON.stringify({ name: internal.db._name() });
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_statistics
/// @brief returns system status information for the server
///
/// @RESTHEADER{GET /_admin/statistics,reads the statistics}
///
/// @RESTDESCRIPTION
///
/// Returns the statistics information. The returned object contains the
/// statistics figures grouped together according to the description returned by
/// `_admin/statistics-description`. For instance, to access a figure `userTime`
/// from the group `system`, you first select the sub-object describing the
/// group stored in `system` and in that sub-object the value for `userTime` is
/// stored in the attribute of the same name.
///
/// In case of a distribution, the returned object contains the total count in
/// `count` and the distribution list in `counts`. The sum (or total) of the
/// individual values is returned in `sum`.
/// 
/// @RESTRETURNCODES
/// 
/// @RESTRETURNCODE{200}
/// Statistics were returned successfully.
/// 
/// @EXAMPLES
/// 
/// @EXAMPLE_ARANGOSH_RUN{RestAdminStatistics1}
///     var url = "/_admin/statistics";
///     var response = logCurlRequest('GET', url);
/// 
///     assert(response.code === 200);
/// 
///     logJsonResponse(response);
/// @END_EXAMPLE_ARANGOSH_RUN
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/statistics",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    var result;

    try {
      result = {};
      result.system = internal.processStatistics();
      result.client = internal.clientStatistics();
      result.http = internal.httpStatistics();
      result.server = internal.serverStatistics();

      actions.resultOk(req, res, actions.HTTP_OK, result);
    }
    catch (err) {
      actions.resultException(req, res, err, undefined, false);
    }
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_statistics_description
/// @brief returns statistics description
///
/// @RESTHEADER{GET /_admin/statistics-description,statistics description}
/// 
/// @RESTDESCRIPTION
///
/// Returns a description of the statistics returned by `/_admin/statistics`.
/// The returned objects contains a list of statistics groups in the attribute
/// `groups` and a list of statistics figures in the attribute `figures`.
///
/// A statistics group is described by
///
/// - `group`: The identifier of the group.
/// - `name`: The name of the group.
/// - `description`: A description of the group.
///
/// A statistics figure is described by
///
/// - `group`: The identifier of the group to which this figure belongs.
/// - `identifier`: The identifier of the figure. It is unique within the group.
/// - `name`: The name of the figure.
/// - `description`: A description of the figure.
/// - `type`: Either `current`, `accumulated`, or `distribution`.
/// - `cuts`: The distribution vector.
/// - `units`: Units in which the figure is measured.
///
/// @RESTRETURNCODES
/// 
/// @RESTRETURNCODE{200}
/// Description was returned successfully.
/// 
/// @EXAMPLES
/// 
/// @EXAMPLE_ARANGOSH_RUN{RestAdminStatisticsDescription1}
///     var url = "/_admin/statistics-description";
///     var response = logCurlRequest('GET', url);
/// 
///     assert(response.code === 200);
/// 
///     logJsonResponse(response);
/// @END_EXAMPLE_ARANGOSH_RUN
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/statistics-description",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    var result;

    try {
      result = {
        groups: [
          {
            group: "system",
            name: "Process Statistics",
            description: "Statistics about the ArangoDB process"
          },

          {
            group: "client",
            name: "Client Connection Statistics",
            description: "Statistics about the connections."
          },
          
          {
            group: "http",
            name: "HTTP Request Statistics",
            description: "Statistics about the HTTP requests."
          },
          
          {
            group: "server",
            name: "Server Statistics",
            description: "Statistics about the ArangoDB server"
          }

        ],

        figures: [

          // .............................................................................
          // system statistics
          // .............................................................................

          {
            group: "system",
            identifier: "userTime",
            name: "User Time",
            description: "Amount of time that this process has been scheduled in user mode, " + 
                         "measured in clock ticks divided by sysconf(_SC_CLK_TCK) aka seconds.",
            type: "accumulated",
            units: "seconds"
          },

          {
            group: "system",
            identifier: "systemTime",
            name: "System Time",
            description: "Amount of time that this process has been scheduled in kernel mode, " +
                         "measured in clock ticks divided by sysconf(_SC_CLK_TCK) aka seconds.",
            type: "accumulated",
            units: "seconds"
          },

          {
            group: "system",
            identifier: "numberOfThreads",
            name: "Number of Threads",
            description: "Number of threads in this process.",
            type: "current",
            units: "number"
          },

          {
            group: "system",
            identifier: "residentSize",
            name: "Resident Set Size",
            description: "The total size of the number of pages the process has in real memory. " + 
                         "This is just the pages which count toward text, data, or stack space. " +
                         "This does not include pages which have not been demand-loaded in, " +
                         "or which are swapped out. The resident set size is reported in bytes.",
            type: "current",
            units: "bytes"
          },

          {
            group: "system",
            identifier: "virtualSize",
            name: "Virtual Memory Size",
            description: "The size of the virtual memory the process is using.",
            type: "current",
            units: "bytes"
          },

          {
            group: "system",
            identifier: "minorPageFaults",
            name: "Minor Page Faults",
            description: "The number of minor faults the process has made which have " +
                         "not required loading a memory page from disk.",
            type: "accumulated",
            units: "number"
          },

          {
            group: "system",
            identifier: "majorPageFaults",
            name: "Major Page Faults",
            description: "The number of major faults the process has made which have required " +
                         "loading a memory page from disk.",
            type: "accumulated",
            units: "number"
          },

          // .............................................................................
          // client statistics
          // .............................................................................
          
          {
            group: "client",
            identifier: "httpConnections",
            name: "Client Connections",
            description: "The number of connections that are currently open.",
            type: "current",
            units: "number"
          },

          {
            group: "client",
            identifier: "totalTime",
            name: "Total Time",
            description: "Total time needed to answer a request.",
            type: "distribution",
            cuts: internal.requestTimeDistribution,
            units: "seconds"
          },

          {
            group: "client",
            identifier: "requestTime",
            name: "Request Time",
            description: "Request time needed to answer a request.",
            type: "distribution",
            cuts: internal.requestTimeDistribution,
            units: "seconds"
          },

          {
            group: "client",
            identifier: "queueTime",
            name: "Queue Time",
            description: "Queue time needed to answer a request.",
            type: "distribution",
            cuts: internal.requestTimeDistribution,
            units: "seconds"
          },
          
          {
            group: "client",
            identifier: "bytesSent",
            name: "Bytes Sent",
            description: "Bytes sents for a request.",
            type: "distribution",
            cuts: internal.bytesSentDistribution,
            units: "bytes"
          },

          {
            group: "client",
            identifier: "bytesReceived",
            name: "Bytes Received",
            description: "Bytes receiveds for a request.",
            type: "distribution",
            cuts: internal.bytesReceivedDistribution,
            units: "bytes"
          },

          {
            group: "client",
            identifier: "connectionTime",
            name: "Connection Time",
            description: "Total connection time of a client.",
            type: "distribution",
            cuts: internal.connectionTimeDistribution,
            units: "seconds"
          },
          
          {
            group: "http",
            identifier: "requestsTotal",
            name: "Total requests",
            description: "Total number of HTTP requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsAsync",
            name: "Async requests",
            description: "Number of asynchronously executed HTTP requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsGet",
            name: "HTTP GET requests",
            description: "Number of HTTP GET requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsHead",
            name: "HTTP HEAD requests",
            description: "Number of HTTP HEAD requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsPost",
            name: "HTTP POST requests",
            description: "Number of HTTP POST requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsPut",
            name: "HTTP PUT requests",
            description: "Number of HTTP PUT requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsPatch",
            name: "HTTP PATCH requests",
            description: "Number of HTTP PATCH requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsDelete",
            name: "HTTP DELETE requests",
            description: "Number of HTTP DELETE requests.",
            type: "accumulated",
            units: "number"
          },
          
          {
            group: "http",
            identifier: "requestsOptions",
            name: "HTTP OPTIONS requests",
            description: "Number of HTTP OPTIONS requests.",
            type: "accumulated",
            units: "number"
          },

          // .............................................................................
          // server statistics
          // .............................................................................

          {
            group: "server",
            identifier: "uptime",
            name: "Server Uptime",
            description: "Number of seconds elapsed since server start.",
            type: "current",
            units: "seconds"
          }

        ]
      };

      actions.resultOk(req, res, actions.HTTP_OK, result);
    }
    catch (err) {
      actions.resultException(req, res, err, undefined, false);
    }
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_post_admin_test
/// @brief executes one or multiple tests on the server
///
/// @RESTHEADER{POST /_admin/test,runs tests on the server}
///
/// @RESTBODYPARAM{body,javascript,required}
/// A JSON body containing an attribute "tests" which lists the files 
/// containing the test suites.
///
/// @RESTDESCRIPTION
///
/// Executes the specified tests on the server and returns an object with the
/// test results. The object has an attribute "error" which states whether 
/// any error occurred. The object also has an attribute "passed" which 
/// indicates which tests passed and which did not.
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/test",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    var body = actions.getJsonBody(req, res);

    if (body === undefined) {
      return;
    }
   
    var tests = body.tests;
    if (! Array.isArray(tests)) {
      actions.resultError(req, res,
                          actions.HTTP_BAD, arangodb.ERROR_HTTP_BAD_PARAMETER,
                          "expected attribute 'tests' is missing");
      return;
    }

    var jsUnity = require("jsunity");
    var testResults = { passed: { }, error: false };
    
    tests.forEach (function (test) {
      var result = false;
      try {
        result = jsUnity.runTest(test);
      }
      catch (err) {
      }
      testResults.passed[test] = result;
      if (! result) {
        testResults.error = true;
      }
    });

    actions.resultOk(req, res, actions.HTTP_OK, testResults);
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_get_admin_execute
/// @brief executes a JavaScript program on the server
///
/// @RESTHEADER{POST /_admin/execute,executes a program}
///
/// @RESTBODYPARAM{body,javascript,required}
/// The body to be executed.
///
/// @RESTDESCRIPTION
///
/// Executes the javascript code in the body on the server.
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/execute",
  context : "admin",
  prefix : false,

  callback : function (req, res) {
    var body = req.requestBody;
    var result;

    console.warn("about to execute: '%s'", body);

    if (body !== "") {
      result = eval("(function() {" + body + "}());");
    }

    actions.resultOk(req, res, actions.HTTP_OK, JSON.stringify(result));
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_GET
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{GET /_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTDESCRIPTION
///
/// Executes a cluster roundtrip from a coordinator to a DB server and
/// back. This call only works in a coordinator node in a cluster.
/// One can and should append an arbitrary path to the URL and the
/// part after `/_admin/sharding-test` is used as the path of the HTTP
/// request which is sent from the coordinator to a DB node. Likewise,
/// any form data appended to the URL is forwarded in the request to the
/// DB node. This handler takes care of all request types (see below)
/// and uses the same request type in its request to the DB node.
///
/// The following HTTP headers are interpreted in a special way:
///
///   - `X-Shard-ID`: This specifies the ID of the shard to which the
///     cluster request is sent and thus tells the system to which DB server
///     to send the cluster request. Note that the mapping from the 
///     shard ID to the responsible server has to be defined in the 
///     agency under `Current/ShardLocation/<shardID>`. One has to give
///     this header, otherwise the system does not know where to send
///     the request.
///   - `X-Client-Transaction-ID`: the value of this header is taken
///     as the client transaction ID for the request
///   - `X-Timeout`: specifies a timeout in seconds for the cluster
///     operation. If the answer does not arrive within the specified
///     timeout, an corresponding error is returned and any subsequent
///     real answer is ignored. The default if not given is 24 hours.
///   - `X-Synchronous-Mode`: If set to `true` the test function uses
///     synchronous mode, otherwise the default asynchronous operation
///     mode is used. This is mainly for debugging purposes.
///   - `Host`: This header is ignored and not forwarded to the DB server.
///   - `User-Agent`: This header is ignored and not forwarded to the DB 
///     server.
///
/// All other HTTP headers and the body of the request (if present, see
/// other HTTP methods below) are forwarded as given in the original request.
///
/// In asynchronous mode the DB server answers with an HTTP request of its
/// own, in synchronous mode it sends a HTTP response. In both cases the
/// headers and the body are used to produce the HTTP response of this
/// API call.
///
/// @RESTRETURNCODES
///
/// The return code can be anything the cluster request returns, as well as:
///
/// @RESTRETURNCODE{200}
/// is returned when everything went well, or if a timeout occurred. In the
/// latter case a body of type application/json indicating the timeout
/// is returned.
/// 
/// @RESTRETURNCODE{403}
/// is returned if ArangoDB is not running in cluster mode.
/// 
/// @RESTRETURNCODE{404}
/// is returned if ArangoDB was not compiled for cluster operation.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_POST
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{POST /_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTBODYPARAM{body,anything,required}
///
/// @RESTDESCRIPTION
/// See GET method. The body can be any type and is simply forwarded.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_PUT
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{PUT /_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTBODYPARAM{body,anything,required}
///
/// @RESTDESCRIPTION
/// See GET method. The body can be any type and is simply forwarded.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_DELETE
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{DELETE /_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTDESCRIPTION
/// See GET method. The body can be any type and is simply forwarded.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_PATCH
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{PATCH/_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTBODYPARAM{body,anything,required}
///
/// @RESTDESCRIPTION
/// See GET method. The body can be any type and is simply forwarded.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_sharding_test_HEAD
/// @brief executes a cluster roundtrip for sharding
///
/// @RESTHEADER{HEAD/_admin/sharding-test,executes a cluster roundtrip}
///
/// @RESTDESCRIPTION
/// See GET method. The body can be any type and is simply forwarded.
////////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url : "_admin/sharding-test",
  context : "admin",
  prefix : true,

  callback : function (req, res) {
    var path;
    if (req.hasOwnProperty('suffix') && req.suffix.length !== 0) {
      path = "/"+req.suffix.join("/");
    }
    else {
      path = "/_admin/version";
    }
    var params = "";
    var shard = "";
    var p;

    for (p in req.parameters) {
      if (req.parameters.hasOwnProperty(p)) {
        if (params === "") {
          params = "?";
        }
        else {
          params += "&";
        }
        params += p+"="+String(req.parameters[p]);
      }
    }
    if (params !== "") {
      path += params;
    }
    var headers = {};
    var transID = "";
    var timeout = 24*3600.0;
    var asyncMode = true;

    for (p in req.headers) {
      if (req.headers.hasOwnProperty(p)) {
        if (p === "x-client-transaction-id") {
          transID = req.headers[p];
        }
        else if (p === "x-timeout") {
          timeout = parseFloat(req.headers[p]);
          if (isNaN(timeout)) {
            timeout = 24*3600.0;
          }
        }
        else if (p === "x-synchronous-mode") {
          asyncMode = false;
        }
        else if (p === "x-shard-id") {
          shard = req.headers[p];
        } 
        else {
          headers[p] = req.headers[p];
        }
      }
    }

    var body;
    if (req.requestBody === undefined || typeof req.requestBody !== "string") {
      body = "";
    }
    else {
      body = req.requestBody;
    }
    
    var r;
    if (typeof SYS_SHARDING_TEST === "undefined") {
      actions.resultError(req, res, actions.HTTP_NOT_FOUND,
                          "Not compiled for cluster operation");
    }
    else {
      try {
        r = SYS_SHARDING_TEST(req, res, shard, path, transID, 
                              headers, body, timeout, asyncMode);
        if (r.timeout || typeof r.errorMessage === 'string') {
          res.responseCode = actions.HTTP_OK;
          res.contentType = "application/json; charset=utf-8";
          var s = JSON.stringify(r);
          res.body = s;
        }
        else {
          res.responseCode = actions.HTTP_OK;
          res.contentType = r.headers.contentType;
          res.headers = r.headers;
          res.body = r.body;
        }
      }
      catch(err) {
        actions.resultError(req, res, actions.HTTP_FORBIDDEN, String(err));
      }
    }
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// @addtogroup\\|// --SECTION--\\|/// @page\\|/// @\\}"
// End:
