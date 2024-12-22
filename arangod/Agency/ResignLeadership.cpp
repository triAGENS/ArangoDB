////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "Agency/ResignLeadership.h"

#include "Agency/AgentInterface.h"
#include "Agency/Helpers.h"
#include "Agency/Job.h"
#include "Agency/JobContext.h"
#include "Agency/MoveShard.h"
#include "Agency/Node.h"
#include "Agency/NodeDeserialization.h"
#include "Agency/ReconfigureReplicatedLog.h"
#include "Basics/TimeString.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"

using namespace arangodb::consensus;
using namespace arangodb::velocypack;

ResignLeadership::ResignLeadership(Node const& snapshot, AgentInterface* agent,
                                   std::string const& jobId,
                                   std::string const& creator,
                                   std::string const& server)
    : Job(NOTFOUND, snapshot, agent, jobId, creator), _server(id(server)) {}

ResignLeadership::ResignLeadership(Node const& snapshot, AgentInterface* agent,
                                   JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  std::string path = pos[status] + _jobId + "/";
  auto tmp_server = _snapshot.hasAsString(path + "server");
  auto tmp_creator = _snapshot.hasAsString(path + "creator");
  auto tmp_undoMoves = _snapshot.hasAsBool(path + "undoMoves");
  auto tmp_waitForInSync = _snapshot.hasAsBool(path + "waitForInSync");
  auto tmp_waitForInSyncTimeout =
      _snapshot.hasAsUInt(path + "waitForInSyncTimeout");

  if (tmp_server && tmp_creator) {
    _server = tmp_server.value();
    _creator = tmp_creator.value();
    _undoMoves = tmp_undoMoves.value_or(true);
    _waitForInSync = tmp_waitForInSync.value_or(false);
    _waitForInSyncTimeout = tmp_waitForInSyncTimeout.value_or(0);
  } else {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency.";
    LOG_TOPIC("dead5", ERR, Logger::SUPERVISION) << err.str();
    finish(tmp_server.value(), "", false, err.str());
    _status = FAILED;
  }
}

ResignLeadership::~ResignLeadership() = default;

void ResignLeadership::run(bool& aborts) { runHelper(_server, "", aborts); }

JOB_STATUS ResignLeadership::status() {
  if (_status != PENDING) {
    return _status;
  }

  auto const& todos = *_snapshot.hasAsChildren(toDoPrefix);
  auto const& pends = *_snapshot.hasAsChildren(pendingPrefix);
  size_t found = 0;

  for (auto const& subJob : todos) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      found++;
    }
  }
  for (auto const& subJob : pends) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      found++;
    }
  }

  if (found > 0) {  // some subjob still running
    // timeout here:
    auto tmp_time =
        _snapshot.hasAsString(pendingPrefix + _jobId + "/timeCreated");
    std::string timeCreatedString = tmp_time.value();
    Supervision::TimePoint timeCreated = stringToTimepoint(timeCreatedString);
    Supervision::TimePoint now(std::chrono::system_clock::now());
    if (now - timeCreated > std::chrono::duration<double>(86400.0)) {  // 1 day
      abort("timed out");
      return FAILED;
    }
    return PENDING;
  }

  Node::Children const& failed = *_snapshot.hasAsChildren(failedPrefix);
  size_t failedFound = 0;
  for (auto const& subJob : failed) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      failedFound++;
    }
  }

  if (failedFound > 0) {
    abort("failed found");
    return FAILED;
  }

  // all subjobs done:

  // Put server in /Target/CleanedServers:
  Builder reportTrx;
  {
    VPackArrayBuilder arrayGuard(&reportTrx);
    {
      VPackObjectBuilder objectGuard(&reportTrx);
      reportTrx.add(VPackValue(toBeCleanedPrefix));
      {
        VPackObjectBuilder guard4(&reportTrx);
        reportTrx.add("op", VPackValue("erase"));
        reportTrx.add("val", VPackValue(_server));
      }
      addRemoveJobFromSomewhere(reportTrx, "Pending", _jobId);
      Builder job;
      std::ignore = _snapshot.hasAsBuilder(pendingPrefix + _jobId, job);
      addPutJobIntoSomewhere(reportTrx, "Finished", job.slice(), "");
      addReleaseServer(reportTrx, _server);
    }
  }

  // Transact to agency
  write_ret_t res = singleWriteTransaction(_agent, reportTrx, false);

  if (res.accepted && res.indices.size() == 1 && res.indices[0] != 0) {
    return FINISHED;
  }

  LOG_TOPIC("dead6", ERR, Logger::SUPERVISION) << "Failed to report " << _jobId;
  return FAILED;
}

bool ResignLeadership::create(std::shared_ptr<VPackBuilder> envelope) {
  LOG_TOPIC("dead7", DEBUG, Logger::SUPERVISION)
      << "Todo: Resign leadership server " << _server;

  bool selfCreate = (envelope == nullptr);  // Do we create ourselves?

  if (selfCreate) {
    _jb = std::make_shared<Builder>();
  } else {
    _jb = envelope;
  }

  std::string path = toDoPrefix + _jobId;

  {
    VPackArrayBuilder guard(_jb.get());
    VPackObjectBuilder guard2(_jb.get());
    _jb->add(VPackValue(path));
    {
      VPackObjectBuilder guard3(_jb.get());
      _jb->add("type", VPackValue("resignLeadership"));
      _jb->add("server", VPackValue(_server));
      _jb->add("jobId", VPackValue(_jobId));
      _jb->add("creator", VPackValue(_creator));
      _jb->add("undoMoves", VPackValue(_undoMoves));
      _jb->add("waitForInSync", VPackValue(_waitForInSync));
      _jb->add("waitForInSyncTimeout", VPackValue(_waitForInSyncTimeout));
      _jb->add("timeCreated",
               VPackValue(timepointToString(std::chrono::system_clock::now())));
    }
  }

  _status = TODO;

  if (!selfCreate) {
    return true;
  }

  write_ret_t res = singleWriteTransaction(_agent, *_jb, false);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    return true;
  }

  _status = NOTFOUND;

  LOG_TOPIC("dead8", INFO, Logger::SUPERVISION)
      << "Failed to insert job " << _jobId;
  return false;
}

bool ResignLeadership::start(bool& aborts) {
  // If anything throws here, the run() method catches it and finishes
  // the job.

  // Check if the server exists:
  if (!_snapshot.has(plannedServers + "/" + _server)) {
    finish("", "", false, "server does not exist as DBServer in Plan");
    return false;
  }

  // Check that the server is not locked:
  if (_snapshot.has(blockedServersPrefix + _server)) {
    LOG_TOPIC("dead9", DEBUG, Logger::SUPERVISION)
        << "server " << _server
        << " is currently locked, not starting ResignLeadership job " << _jobId;
    return false;
  }

  // Check that the server is in state "GOOD":
  std::string health = checkServerHealth(_snapshot, _server);
  if (health != Supervision::HEALTH_STATUS_GOOD) {
    LOG_TOPIC("deada", DEBUG, Logger::SUPERVISION)
        << "server " << _server << " is currently " << health
        << ", not starting ResignLeadership job " << _jobId;
    return false;
  }

  // Check that _to is not in `Target/CleanedServers`:
  VPackBuilder cleanedServersBuilder;
  auto const& cleanedServersNode = _snapshot.get(cleanedPrefix);
  if (cleanedServersNode) {
    cleanedServersNode->toBuilder(cleanedServersBuilder);
  } else {
    // ignore this check
    cleanedServersBuilder.clear();
    { VPackArrayBuilder guard(&cleanedServersBuilder); }
  }
  VPackSlice cleanedServers = cleanedServersBuilder.slice();
  if (cleanedServers.isArray()) {
    for (VPackSlice x : VPackArrayIterator(cleanedServers)) {
      if (x.isString() && x.stringView() == _server) {
        finish("", "", false, "server must not be in `Target/CleanedServers`");
        return false;
      }
    }
  }

  // Check that _to is not in `Target/FailedServers`:
  //  (this node is expected to NOT exists, so make test before processing
  //   so that get does not generate a warning log message)
  VPackBuilder failedServersBuilder;
  if (_snapshot.has(failedServersPrefix)) {
    auto const& failedServersNode = _snapshot.get(failedServersPrefix);
    if (failedServersNode) {
      failedServersNode->toBuilder(failedServersBuilder);
    } else {
      // ignore this check
      failedServersBuilder.clear();
      { VPackObjectBuilder guard(&failedServersBuilder); }
    }
  } else {
    // ignore this check
    failedServersBuilder.clear();
    { VPackObjectBuilder guard(&failedServersBuilder); }
  }  // if

  VPackSlice failedServers = failedServersBuilder.slice();
  if (failedServers.isObject()) {
    Slice found = failedServers.get(_server);
    if (!found.isNone()) {
      finish("", "", false, "server must not be in `Target/FailedServers`");
      return false;
    }
  }

  // Check if we can get things done in the first place
  if (!checkFeasibility()) {
    finish("", "", false, "server " + _server + " cannot resign");
    return false;
  }

  // Copy todo to pending
  auto pending = std::make_shared<Builder>();
  Builder todo;

  // Get todo entry
  {
    VPackArrayBuilder guard(&todo);
    // When create() was done with the current snapshot, then the job object
    // will not be in the snapshot under ToDo, but in this case we find it
    // in _jb:
    if (_jb == nullptr) {
      auto tmpTodo = _snapshot.hasAsBuilder(toDoPrefix + _jobId, todo);
      if (!tmpTodo) {
        // Just in case, this is never going to happen, since we will only
        // call the start() method if the job is already in ToDo.
        LOG_TOPIC("deadb", INFO, Logger::SUPERVISION)
            << "Failed to get key " << toDoPrefix << _jobId
            << " from agency snapshot";
        return false;
      }
    } else {
      try {
        todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
      } catch (std::exception const& e) {
        // Just in case, this is never going to happen, since when _jb is
        // set, then the current job is stored under ToDo.
        LOG_TOPIC("deadc", WARN, Logger::SUPERVISION) << e.what();
        return false;
      }
    }
  }

  // Enter pending, remove todo, block toserver
  {
    VPackArrayBuilder listOfTransactions(pending.get());

    {
      VPackObjectBuilder objectForMutation(pending.get());

      addPutJobIntoSomewhere(*pending, "Pending", todo.slice()[0]);
      addRemoveJobFromSomewhere(*pending, "ToDo", _jobId);

      addBlockServer(*pending, _server, _jobId);

      // Put ourselves in list of servers to be cleaned:
      pending->add(VPackValue(toBeCleanedPrefix));
      {
        VPackObjectBuilder guard4(pending.get());
        pending->add("op", VPackValue("push"));
        pending->add("new", VPackValue(_server));
      }

      // Schedule shard relocations
      if (!scheduleMoveShards(pending)) {
        LOG_TOPIC("d4473", DEBUG, Logger::SUPERVISION)
            << "Not starting resign leadership job because some shards have no "
               "common in sync follower";
        // check if a timeout value is specified
        if (_waitForInSyncTimeout > 0) {
          auto tmp_time =
              _snapshot.hasAsString(pendingPrefix + _jobId + "/timeCreated");
          std::string timeCreatedString = tmp_time.value();
          Supervision::TimePoint timeCreated =
              stringToTimepoint(timeCreatedString);
          Supervision::TimePoint now(std::chrono::system_clock::now());
          if (now - timeCreated >=
              std::chrono::seconds{_waitForInSyncTimeout}) {
            LOG_TOPIC("d4475", ERR, Logger::SUPERVISION)
                << "Failing resign leadership job (" << _jobId
                << ") because some shards have no common in sync follower";
            finish("", "", false,
                   "Some shards failed to have an in sync follower after "
                   "timeout.");
            return false;
          }
        }
        return false;
      }

    }  // mutation part of transaction done

    // Preconditions
    {
      VPackObjectBuilder objectForPrecondition(pending.get());
      addPreconditionServerNotBlocked(*pending, _server);
      addPreconditionServerHealth(*pending, _server,
                                  Supervision::HEALTH_STATUS_GOOD);
      addPreconditionUnchanged(*pending, failedServersPrefix, failedServers);
      addPreconditionUnchanged(*pending, cleanedPrefix, cleanedServers);
    }
  }  // array for transaction done

  // Transact to agency
  write_ret_t res = singleWriteTransaction(_agent, *pending, false);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC("deadd", DEBUG, Logger::SUPERVISION)
        << "Pending: Clean out server " << _server;

    return true;
  }

  LOG_TOPIC("deade", INFO, Logger::SUPERVISION)
      << "Precondition failed for starting ResignLeadership job " << _jobId;

  return false;
}

void ResignLeadership::scheduleJobsR2(std::shared_ptr<Builder>& trx,
                                      DatabaseID const& database, size_t& sub) {
  auto replicatedLogsPath =
      basics::StringUtils::concatT("/Target/ReplicatedLogs/", database);
  auto logs = _snapshot.hasAsChildren(replicatedLogsPath);

  for (auto const& [logIdString, logNode] : *logs) {
    auto logTarget = deserialize<replication2::agency::LogTarget>(logNode);
    auto logPlan = readLogPlan(_snapshot, database, logTarget.id);

    bool changeLeader = [&] {
      if (logTarget.leader == _server) {
        return true;
      }
      if (logPlan && logPlan->currentTerm && logPlan->currentTerm->leader) {
        return logPlan->currentTerm->leader->serverId == _server;
      }
      return false;
    }();

    if (changeLeader) {
      auto replacement =
          findOtherHealthyParticipant(_snapshot, logTarget, {_server});

      if (replacement.empty()) {
        continue;
      }

      auto undo =
          _undoMoves ? std::optional<std::string>{_server} : std::nullopt;
      ReconfigureReplicatedLog(
          _snapshot, _agent, _jobId + "-" + std::to_string(sub++), _jobId,
          database, logTarget.id,
          {ReconfigureOperation{
              ReconfigureOperation::SetLeader{.participant = replacement}}},
          // cppcheck-suppress accessMoved
          std::move(undo))
          .create(trx);
    }
  }
}

bool ResignLeadership::scheduleMoveShards(std::shared_ptr<Builder>& trx) {
  std::vector<std::string> servers = availableServers(_snapshot);

  Node::Children const& databaseProperties =
      *_snapshot.hasAsChildren(planDBPrefix);
  Node::Children const& databases = *_snapshot.hasAsChildren(planColPrefix);
  size_t sub = 0;

  for (auto const& database : databases) {
    bool const isRepl2 = isReplicationTwoDB(databaseProperties, database.first);
    if (isRepl2) {
      scheduleJobsR2(trx, database.first, sub);
      continue;
    }

    // Find shardsLike dependencies
    for (auto const& collptr : database.second->children()) {
      auto const& collection = *(collptr.second);

      if (collection.has("distributeShardsLike")) {
        continue;
      }

      for (auto const& shard : *collection.hasAsChildren("shards")) {
        // Only shards, which are affected
        int found = -1;
        int count = 0;
        for (VPackSlice dbserver : *shard.second->getArray()) {
          if (dbserver.stringView() == _server) {
            found = count;
            break;
          }
          count++;
        }

        if (found == -1) {
          continue;
        }

        bool isLeader = (found == 0);
        if (isLeader) {
          std::string toServer = Job::findNonblockedCommonHealthyInSyncFollower(
              _snapshot, database.first, collptr.first, shard.first, _server);

          if (toServer.empty()) {
            LOG_TOPIC("5c92e", INFO, Logger::SUPERVISION)
                << "ResignLeadership job: " << _jobId << " server: " << _server
                << "no common in-sync follower found for shard "
                << database.first << "/" << collptr.first << "/" << shard.first
                << (_waitForInSync ? " - waiting for follower."
                                   : " - ignoring.");
            if (_waitForInSync) {
              return false;
            }
            continue;  // can not resign from that shard
          }

          MoveShard(_snapshot, _agent, _jobId + "-" + std::to_string(sub++),
                    _jobId, database.first, collptr.first, ShardID{shard.first},
                    _server, toServer, isLeader, /*remainsFollower*/ true,
                    _undoMoves)
              .withParent(_jobId)
              .create(trx);

        } else {
          // Intentionally do nothing. RemoveServer will remove the failed
          // follower
          LOG_TOPIC("deadf", DEBUG, Logger::SUPERVISION)
              << "Do nothing for resign leadership of follower of the "
                 "collection "
              << collection.hasAsString("id").value();
          continue;
        }
      }
    }
  }

  return true;
}

bool ResignLeadership::checkFeasibility() {
  std::vector<std::string> availServers = availableServers(_snapshot);

  // Minimum 1 DB server must remain:
  if (availServers.size() == 1) {
    LOG_TOPIC("deaa0", ERR, Logger::SUPERVISION)
        << "DB server " << _server << " is the last standing db server.";
    return false;
  }

  return true;
}

arangodb::Result ResignLeadership::abort(std::string const& reason) {
  // We can assume that the job is either in ToDo or in Pending.
  Result result;

  if (_status == NOTFOUND || _status == FINISHED || _status == FAILED) {
    result = Result(TRI_ERROR_SUPERVISION_GENERAL_FAILURE,
                    "Failed aborting failedServer beyond pending stage");
    return result;
  }

  // Can now only be TODO or PENDING
  if (_status == TODO) {
    finish("", "", false, "job aborted: " + reason);
    return result;
  }

  // Abort all our subjobs:
  Node::Children const& todos = *_snapshot.hasAsChildren(toDoPrefix);
  Node::Children const& pends = *_snapshot.hasAsChildren(pendingPrefix);

  std::string moveShardAbortReason = "resign leadership aborted: " + reason;

  for (auto const& subJob : todos) {
    if (subJob.first.compare(0, _jobId.size() + 1, _jobId + "-") == 0) {
      JobContext(TODO, subJob.first, _snapshot, _agent)
          .abort(moveShardAbortReason);
    }
  }
  for (auto const& subJob : pends) {
    if (subJob.first.compare(0, _jobId.size() + 1, _jobId + "-") == 0) {
      JobContext(PENDING, subJob.first, _snapshot, _agent)
          .abort(moveShardAbortReason);
    }
  }

  auto payload = std::make_shared<VPackBuilder>();
  {
    VPackObjectBuilder p(payload.get());
    payload->add(VPackValue(toBeCleanedPrefix));
    {
      VPackObjectBuilder pp(payload.get());
      payload->add("op", VPackValue("erase"));
      payload->add("val", VPackValue(_server));
    }
  }

  finish(_server, "", false, "job aborted: " + reason, payload);

  return result;
}
