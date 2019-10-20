/* jshint strict: false, unused: false */
/* global ArangoServerState, ArangoClusterComm, ArangoClusterInfo, ArangoAgency */

// //////////////////////////////////////////////////////////////////////////////
// / @brief cluster actions
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
// / @author Max Neunhoeffer
// / @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
// / @author Copyright 2013-2014, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const actions = require('@arangodb/actions');
const cluster = require('@arangodb/cluster');
const users = require('@arangodb/users');
const wait = require("internal").wait;
const _ = require('lodash');

actions.defineHttp({
  url: '_admin/cluster/removeServer',
  allowUseDatabase: true,
  prefix: false,
  isSystem: true,

  callback: function (req, res) {
    if (req.requestType !== actions.POST || !cluster.isCoordinator()) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only DELETE requests are allowed and only to coordinators');
      return;
    }

    if (req.database !== '_system' || !req.isAdminUser) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only allowed for admins on the _system database');
      return;
    }

    let serverId;
    try {
      serverId = JSON.parse(req.requestBody);
    } catch (e) {
    }

    if (typeof serverId !== 'string') {
      actions.resultError(req, res, actions.HTTP_BAD,
        'required parameter ServerID was not given');
      return;
    }

    let agency = ArangoAgency.get('', false, true).arango;

    let node = agency.Supervision.Health[serverId];
    if (node === undefined) {
      actions.resultError(req, res, actions.HTTP_NOT_FOUND,
        'unknown server id');
      return;
    }

    if (serverId.substr(0, 4) !== 'CRDN' && serverId.substr(0, 4) !== 'PRMR') {
      actions.resultError(req, res, actions.HTTP_BAD,
        'couldn\'t determine role for serverid ' + serverId);
      return;
    }

    let count = 0;    // Try for 60s if server still in use or not failed
    let msg = "";
    let used = [];
    while (++count <= 60) {
      // need to make sure it is not responsible for anything
      used = [];
      let preconditions = reducePlanServers(function (data, agencyKey, servers) {
        data[agencyKey] = {'old': servers};
        if (servers.indexOf(serverId) !== -1) {
          used.push(agencyKey);
        }
        return data;
      }, {});
      preconditions = reduceCurrentServers(function (data, agencyKey, servers) {
        data[agencyKey] = {'old': servers};
        if (servers.indexOf(serverId) !== -1) {
          used.push(agencyKey);
        }
        return data;
      }, preconditions);

      preconditions['/arango/Supervision/Health/' + serverId + '/Status'] = {'old': 'FAILED'};
      preconditions["/arango/Supervision/DBServers/" + serverId]
        = { "oldEmpty": true };

      if (!checkServerLocked(serverId) && used.length === 0) {
        let operations = {};
        operations['/arango/Plan/Coordinators/' + serverId] = {'op': 'delete'};
        operations['/arango/Plan/DBServers/' + serverId] = {'op': 'delete'};
        operations['/arango/Current/ServersRegistered/' + serverId] = {'op': 'delete'};
        operations['/arango/Current/DBServers/' + serverId] = {'op': 'delete'};
        operations['/arango/Supervision/Health/' + serverId] = {'op': 'delete'};
        operations['/arango/Target/MapUniqueToShortID/' + serverId] = {'op': 'delete'};
        operations['/arango/Current/ServersKnown/' + serverId] = {'op': 'delete'};
        operations['/arango/Target/RemovedServers/' + serverId] = {'op': 'set', 'new': (new Date()).toISOString()};

        try {
          global.ArangoAgency.write([[operations, preconditions]]);
          actions.resultOk(req, res, actions.HTTP_OK, true);
          console.info("Removed server " + serverId + " from cluster");
          return;
        } catch (e) {
          if (e.code === 412) {
            console.log("removeServer: got precondition failed, retrying...");
          } else {
            console.warn("removeServer: could not talk to agency, retrying...");
          }
        }
      } else {
        if (used.length > 0) {
          console.log("removeServer: server", serverId, "still in use in",
                      used.length, "locations.");
        } else {
          console.log("removeServer: server", serverId, "locked in agency.");
        }
      }
      wait(1.0);
    }  // while count
    actions.resultError(req, res, actions.HTTP_PRECONDITION_FAILED,
      'the server not failed, locked or is still in use at the following '
      + 'locations: ' + JSON.stringify(used));
  }
});

// //////////////////////////////////////////////////////////////////////////////
// / @start Docu Block JSF_postMoveShard
// / (intentionally not in manual)
// / @brief triggers activities to move a shard
// /
// / @ RESTHEADER{POST /_admin/cluster/moveShard, Trigger activities to move a shard.}
// /
// / @ RESTQUERYPARAMETERS
// /
// / @ RESTDESCRIPTION Triggers activities to move a shard.
// / The body must be a JSON document with the following attributes:
// /   - `"database"`: a string with the name of the database
// /   - `"collection"`: a string with the name of the collection
// /   - `"shard"`: a string with the name of the shard to move
// /   - `"fromServer"`: a string with the ID of a server that is currently
// /     the leader or a follower for this shard
// /   - `"toServer"`: a string with the ID of a server that is currently
// /     not the leader and not a follower for this shard
// /
// / @ RESTRETURNCODES
// /
// / @ RESTRETURNCODE{202} is returned when everything went well and the
// / job is scheduled.
// /
// / @ RESTRETURNCODE{400} body is not valid JSON.
// /
// / @ RESTRETURNCODE{403} server is not a coordinator or method was not POST.
// /
// / @ RESTRETURNCODE{503} the agency operation did not work.
// /
// / @end Docu Block
// //////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url: '_admin/cluster/moveShard',
  allowUseDatabase: false,
  prefix: false,
  isSystem: true,

  callback: function (req, res) {
    if (!cluster.isCoordinator()) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only coordinators can serve this request');
      return;
    }
    if (req.requestType !== actions.POST) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only the POST method is allowed');
      return;
    }

    // Now get to work:
    var body = actions.getJsonBody(req, res);
    if (body === undefined) {
      return;
    }
    if (typeof body !== 'object' ||
      !body.hasOwnProperty('database') ||
      typeof body.database !== 'string' ||
      !body.hasOwnProperty('collection') ||
      typeof body.collection !== 'string' ||
      !body.hasOwnProperty('shard') ||
      typeof body.shard !== 'string' ||
      !body.hasOwnProperty('fromServer') ||
      typeof body.fromServer !== 'string' ||
      !body.hasOwnProperty('toServer') ||
      typeof body.toServer !== 'string') {
      actions.resultError(req, res, actions.HTTP_BAD,
        "body must be an object with string attributes 'database', 'collection', 'shard', 'fromServer' and 'toServer'");
      return;
    }

    // at least RW rights on db to move a shard
    if (!req.isAdminUser &&
        users.permission(req.user, body.database) !== 'rw') {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'insufficent permissions on database to move shard');
      return;
    }

    body.shards = [body.shard];
    body.collections = [body.collection];
    var r = cluster.moveShard(body);
    if (r.error) {
      actions.resultError(req, res, actions.HTTP_SERVICE_UNAVAILABLE, r);
      return;
    }
    actions.resultOk(req, res, actions.HTTP_ACCEPTED, r);
  }
});

// //////////////////////////////////////////////////////////////////////////////
// / @start Docu Block JSF_postRebalanceShards
// / (intentionally not in manual)
// / @brief triggers activities to rebalance shards
// /
// / @ RESTHEADER{POST /_admin/cluster/rebalanceShards, Trigger activities to rebalance shards.}
// /
// / @ RESTDESCRIPTION Triggers activities to rebalance shards.
// / The body must be an empty JSON object.
// /
// / @ RESTRETURNCODES
// /
// / @ RESTRETURNCODE{202} is returned when everything went well.
// /
// / @ RESTRETURNCODE{400} body is not valid JSON.
// /
// / @ RESTRETURNCODE{403} server is not a coordinator or method was not POST.
// /
// / @ RESTRETURNCODE{503} the agency operation did not work.
// /
// / @end Docu Block
// //////////////////////////////////////////////////////////////////////////////

actions.defineHttp({
  url: '_admin/cluster/rebalanceShards',
  allowUseDatabase: true,
  prefix: false,
  isSystem: true,

  callback: function (req, res) {
    if (!cluster.isCoordinator()) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only coordinators can serve this request');
      return;
    }
    if (req.requestType !== actions.POST) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only the POST method is allowed');
      return;
    }

    // simon: RO is sufficient to rebalance shards for current db
    if (req.database !== '_system'/* || !req.isAdminUser*/) {
      actions.resultError(req, res, actions.HTTP_FORBIDDEN, 0,
        'only allowed for admins on the _system database');
      return;
    }

    // Now get to work:
    var body = actions.getJsonBody(req, res);
    if (body === undefined) {
      return;
    }
    if (typeof body !== 'object') {
      actions.resultError(req, res, actions.HTTP_BAD,
        'body must be an object.');
      return;
    }
    var ok = cluster.rebalanceShards();
    if (!ok) {
      actions.resultError(req, res, actions.HTTP_SERVICE_UNAVAILABLE,
        'Cannot write to agency.');
      return;
    }
    actions.resultOk(req, res, actions.HTTP_ACCEPTED, true);
  }
});
