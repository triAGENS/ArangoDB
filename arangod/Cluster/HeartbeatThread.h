////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_HEARTBEAT_THREAD_H
#define ARANGOD_CLUSTER_HEARTBEAT_THREAD_H 1

#include "Basics/Common.h"
#include "Basics/ConditionVariable.h"
#include "Basics/Mutex.h"
#include "Basics/Thread.h"
#include "Basics/Logger.h"
#include "Cluster/AgencyComm.h"

struct TRI_server_t;
struct TRI_vocbase_t;

namespace arangodb {
namespace rest {
class ApplicationDispatcher;
}

class ApplicationV8;

class HeartbeatThread : public Thread {
 private:
  HeartbeatThread(HeartbeatThread const&);
  HeartbeatThread& operator=(HeartbeatThread const&);

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a heartbeat thread
  //////////////////////////////////////////////////////////////////////////////

  HeartbeatThread(TRI_server_t*, arangodb::rest::ApplicationDispatcher*,
                  ApplicationV8*, uint64_t, uint64_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys a heartbeat thread
  //////////////////////////////////////////////////////////////////////////////

  ~HeartbeatThread();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief initializes the heartbeat
  //////////////////////////////////////////////////////////////////////////////

  bool init();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the thread is ready
  //////////////////////////////////////////////////////////////////////////////

  bool isReady();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set the thread status to ready
  //////////////////////////////////////////////////////////////////////////////

  void setReady();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief decrement the counter for a dispatched job, the argument is true
  /// if the job was finished successfully and false otherwise
  //////////////////////////////////////////////////////////////////////////////

  void removeDispatchedJob(bool success);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief check whether a job is still running or does not have reported yet
  //////////////////////////////////////////////////////////////////////////////

  bool hasPendingJob();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the thread has run at least once.
  /// this is used on the coordinator only
  //////////////////////////////////////////////////////////////////////////////

  static bool hasRunOnce() { return (HasRunOnce == 1); }

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief heartbeat main loop
  //////////////////////////////////////////////////////////////////////////////

  void run();

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief heartbeat main loop, coordinator version
  //////////////////////////////////////////////////////////////////////////////

  void runCoordinator();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief heartbeat main loop, dbserver version
  //////////////////////////////////////////////////////////////////////////////

  void runDBServer();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles a plan change, coordinator case
  //////////////////////////////////////////////////////////////////////////////

  bool handlePlanChangeCoordinator(uint64_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles a plan change, DBServer case
  //////////////////////////////////////////////////////////////////////////////

  bool handlePlanChangeDBServer(uint64_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles a state change
  //////////////////////////////////////////////////////////////////////////////

  bool handleStateChange(AgencyCommResult&, uint64_t&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief fetch the last value of Sync/Commands/my-id from the agency
  //////////////////////////////////////////////////////////////////////////////

  uint64_t getLastCommandIndex();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sends the current server's state to the agency
  //////////////////////////////////////////////////////////////////////////////

  bool sendState();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief fetch users for a database (run on coordinator only)
  //////////////////////////////////////////////////////////////////////////////

  bool fetchUsers(TRI_vocbase_t*);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief server
  //////////////////////////////////////////////////////////////////////////////

  TRI_server_t* _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Job dispatcher
  //////////////////////////////////////////////////////////////////////////////

  arangodb::rest::ApplicationDispatcher* _dispatcher;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief v8 dispatcher
  //////////////////////////////////////////////////////////////////////////////

  ApplicationV8* _applicationV8;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief status lock
  //////////////////////////////////////////////////////////////////////////////

  arangodb::Mutex _statusLock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief AgencyComm instance
  //////////////////////////////////////////////////////////////////////////////

  AgencyComm _agency;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief condition variable for heartbeat
  //////////////////////////////////////////////////////////////////////////////

  arangodb::basics::ConditionVariable _condition;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief users for these databases will be re-fetched the next time the
  /// heartbeat thread runs
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_set<TRI_vocbase_t*> _refetchUsers;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief this server's id
  //////////////////////////////////////////////////////////////////////////////

  std::string const _myId;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief heartbeat interval
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _interval;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of fails in a row before a warning is issued
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _maxFailsBeforeWarning;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief current number of fails in a row
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _numFails;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief current number of dispatched (pending) jobs
  //////////////////////////////////////////////////////////////////////////////

  int64_t _numDispatchedJobs;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flag, if last dispatched job was successfull
  //////////////////////////////////////////////////////////////////////////////

  bool _lastDispatchedJobResult;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief version of Plan that triggered the last dispatched job
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _versionThatTriggeredLastJob;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the thread is ready
  //////////////////////////////////////////////////////////////////////////////

  bool _ready;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief stop flag
  //////////////////////////////////////////////////////////////////////////////

  volatile sig_atomic_t _stop;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the heartbeat thread has run at least once
  /// this is used on the coordinator only
  //////////////////////////////////////////////////////////////////////////////

  static volatile sig_atomic_t HasRunOnce;
};
}

#endif
