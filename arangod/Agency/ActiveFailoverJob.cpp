////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "ActiveFailoverJob.h"

#include "Agency/AgentInterface.h"
#include "Agency/Job.h"
#include "Agency/JobContext.h"
#include "Agency/Store.h"
#include "Cluster/ClusterHelpers.h"

using namespace arangodb;
using namespace arangodb::consensus;

ActiveFailoverJob::ActiveFailoverJob(Node const& snapshot, AgentInterface* agent,
                                     std::string const& jobId, std::string const& creator,
                                     std::string const& failed)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _server(failed) { }

ActiveFailoverJob::ActiveFailoverJob(Node const& snapshot, AgentInterface* agent,
                                     JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  try {
    std::string path = pos[status] + _jobId + "/";
    _server = _snapshot.get(path + "server").getString();
    _creator = _snapshot.get(path + "creator").getString();
  } catch (std::exception const& e) {
    std::string err = 
      std::string("Failed to find job ") + _jobId + " in agency: " + e.what();
    LOG_TOPIC(ERR, Logger::SUPERVISION) << err;
    finish(_server, "", false, err);
    _status = FAILED;
  }
}

ActiveFailoverJob::~ActiveFailoverJob() {}

void ActiveFailoverJob::run() {
  runHelper(_server, "");
}

bool ActiveFailoverJob::create(std::shared_ptr<VPackBuilder> envelope) {

  LOG_TOPIC(DEBUG, Logger::SUPERVISION)
    << "Todo: Handle failover for leader " + _server;
  
  bool selfCreate = (envelope == nullptr); // Do we create ourselves?

  if (selfCreate) {
    _jb = std::make_shared<Builder>();
  } else {
    _jb = envelope;
  }
  
  auto now = timepointToString(std::chrono::system_clock::now());  
  { VPackArrayBuilder transaction(_jb.get());
    { VPackObjectBuilder operations(_jb.get());
      // Todo entry
      _jb->add(VPackValue(toDoPrefix + _jobId));
      { VPackObjectBuilder todo(_jb.get());
        _jb->add("creator", VPackValue(_creator));
        _jb->add("type", VPackValue("activeFailover"));
        _jb->add("server", VPackValue(_server));
        _jb->add("jobId", VPackValue(_jobId));
        _jb->add("timeCreated", VPackValue(now));
      } // todo
      
      // FailedServers entry []
      _jb->add(VPackValue(failedServersPrefix + "/" + _server));
      { VPackArrayBuilder failedServers(_jb.get()); }
    } // Operations
    
      //Preconditions
      { VPackObjectBuilder health(_jb.get());
        // Status should still be BAD
        addPreconditionServerHealth(*_jb, _server, Supervision::HEALTH_STATUS_BAD);
        // Target/FailedServers does not already include _server
        _jb->add(VPackValue(failedServersPrefix + "/" + _server));
        { VPackObjectBuilder old(_jb.get());
          _jb->add("oldEmpty", VPackValue(true)); }
        // Target/FailedServers is still as in the snapshot
        _jb->add(VPackValue(failedServersPrefix));
        { VPackObjectBuilder old(_jb.get());
          _jb->add("old", _snapshot(failedServersPrefix).toBuilder().slice());}
      } // Preconditions
    } // transactions
  
  _status = TODO;
  
  if (!selfCreate) {
    return true;
  }
  
  write_ret_t res = singleWriteTransaction(_agent, *_jb);
  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    return true;
  }
  
  _status = NOTFOUND;
  
  LOG_TOPIC(INFO, Logger::SUPERVISION) << "Failed to insert job " + _jobId;
  return false;
}

bool ActiveFailoverJob::start() {
  // If anything throws here, the run() method catches it and finishes
  // the job.
  
  // Fail job, if Health back to not FAILED
  if (checkServerHealth(_snapshot, _server) != Supervision::HEALTH_STATUS_FAILED) {
    std::string reason = "Server " + _server + " is no longer failed. " +
                         "Not starting ActiveFailoverJob job";
    LOG_TOPIC(INFO, Logger::SUPERVISION) << reason;
    finish(_server, "", false, reason);
    return false;
  }
  
  Node const& leader = _snapshot(asyncReplLeader);
  if (leader.compareString(_server) != 0) {
    std::string reason = "Server " + _server + " is not the current replication leader";
    LOG_TOPIC(INFO, Logger::SUPERVISION) << reason;
    finish(_server, "", false, reason);
    return false;
  }
  
  // Abort job blocking server if abortable (should prop never happen)
  try {
    std::string jobId = _snapshot(blockedServersPrefix + _server).getString();
    if (!abortable(_snapshot, jobId)) {
      return false; // job will retry later
    } else {
      JobContext(PENDING, jobId, _snapshot, _agent).abort();
    }
  } catch (...) {}
  
  // Todo entry
  Builder todo;
  { VPackArrayBuilder t(&todo);
    if (_jb == nullptr) {
      try {
        _snapshot(toDoPrefix + _jobId).toBuilder(todo);
      } catch (std::exception const&) {
        LOG_TOPIC(INFO, Logger::SUPERVISION)
        << "Failed to get key " + toDoPrefix + _jobId + " from agency snapshot";
        return false;
      }
    } else {
      todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
    }} // Todo entry
  
  // FIXME do I need to put this into pending ???
  std::string newLeader = findBestFollower(_snapshot);
  if (newLeader.empty()) {
    LOG_TOPIC(INFO, Logger::SUPERVISION) << "No server available, will retry job later";
    return false; // job will retry later
  }
  LOG_TOPIC(INFO, Logger::SUPERVISION) << "Selected '" << newLeader << "' as leader";
  
  // Enter pending, remove todo
  Builder pending;
  { VPackArrayBuilder listOfTransactions(&pending);
    
    { VPackObjectBuilder operations(&pending);
      addPutJobIntoSomewhere(pending, "Finished", todo.slice()[0]);
      addRemoveJobFromSomewhere(pending, "ToDo", _jobId);
      pending.add(asyncReplLeader, VPackValue(newLeader));
    }  // mutation part of transaction done
    
    // Preconditions
    { VPackObjectBuilder precondition(&pending);
      // Failed condition persists
      addPreconditionServerHealth(pending, _server, Supervision::HEALTH_STATUS_FAILED);
      // Destination server still in good condition
      addPreconditionServerHealth(pending, newLeader, Supervision::HEALTH_STATUS_GOOD);
      // Destination server should not be blocked by another job
      addPreconditionServerNotBlocked(pending, newLeader);
      // AsyncReplication leader must be the failed server
      addPreconditionUnchanged(pending, asyncReplLeader, leader.slice());
    }   // precondition done
    
  }  // array for transaction done
  
  // Transact to agency
  write_ret_t res = singleWriteTransaction(_agent, pending);
  
  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    _status = FINISHED;
    LOG_TOPIC(INFO, Logger::SUPERVISION)
    << "Finished: ActiveFailoverJob server " << _server << " failover to " << newLeader;
    return true;
  }
  
  LOG_TOPIC(INFO, Logger::SUPERVISION) << "Precondition failed for ActiveFailoverJob " + _jobId;
  return false;
}

JOB_STATUS ActiveFailoverJob::status() {
  if (_status != PENDING) {
    return _status;
  }
  
  TRI_ASSERT(false);   // PENDING is not an option for this job, since it
  // travels directly from ToDo to Finished or Failed
  return _status;
}

arangodb::Result ActiveFailoverJob::abort() {

  // We can assume that the job is in ToDo or not there:
  if (_status == NOTFOUND || _status == FINISHED || _status == FAILED) {
    return Result(TRI_ERROR_SUPERVISION_GENERAL_FAILURE,
                  "Failed aborting addFollower job beyond pending stage");
  }
  
  Result result;
  // Can now only be TODO or PENDING
  if (_status == TODO) {
    finish("", "", false, "job aborted");
    return result;
  }
  
  TRI_ASSERT(false);  // cannot happen, since job moves directly to FINISHED
  return result;
  
}

typedef std::pair<std::string, std::string> ServerTick;
/// Try to select the follower most in-sync with failed leader
std::string ActiveFailoverJob::findBestFollower(Node const& snapshot) {
  std::vector<std::string> as = healthyServers(snapshot);
  
  // blocked; (not sure if this can even happen)
  try {
    for (auto const& srv : snapshot(blockedServersPrefix).children()) {
      as.erase(std::remove(as.begin(), as.end(), srv.first), as.end());
    }
  } catch (...) {}
  
  std::vector<ServerTick> ticks;
  try { // collect tick values from transient state
    query_t trx = std::make_unique<VPackBuilder>();
    {
      VPackArrayBuilder transactions(trx.get());
      VPackArrayBuilder operations(trx.get());
      trx->add(VPackValue("/" + Job::agencyPrefix + asyncReplPrefix));
    }
    trans_ret_t res = _agent->transient(std::move(trx));
    
    if (res.accepted) {
      VPackSlice resp = res.result->slice();
      if (!resp.isArray() || resp.length() == 0) {
        return "";
      }
      VPackSlice obj = resp.at(0).get({ Job::agencyPrefix, "AsyncReplication"});
      for (VPackObjectIterator::ObjectPair pair : VPackObjectIterator(obj)) {
        std::string srvUUID = pair.key.copyString();
        if (std::find(as.begin(), as.end(), srvUUID) == as.end()) {
          continue; // skip inaccessible servers
        }
        
        VPackSlice leader = pair.value.get("leader"); // broken leader
        VPackSlice lastTick = pair.value.get("lastTick");
        if (leader.isString() && leader.compareString(_server) == 0 &&
            lastTick.isString()) {
          ticks.emplace_back(std::move(srvUUID), lastTick.copyString());
        }
      }
    }
  } catch (...) {}
  
  std::sort(ticks.begin(), ticks.end(), [&](ServerTick const& a,
                                            ServerTick const& b) {
    return b.second.compare(a.second);
  });
  if (!ticks.empty()) {
    return ticks[0].first;
  }
  return ""; // fallback to any available server
}
