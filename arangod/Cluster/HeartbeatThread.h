////////////////////////////////////////////////////////////////////////////////
/// @brief heartbeat thread
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2013, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_CLUSTER_HEARTBEAT_THREAD_H
#define ARANGODB_CLUSTER_HEARTBEAT_THREAD_H 1

#include "Basics/Common.h"
#include "Basics/ConditionVariable.h"
#include "Basics/Mutex.h"
#include "Basics/Thread.h"
#include "Basics/logging.h"
#include "Cluster/AgencyComm.h"

struct TRI_server_t;
struct TRI_vocbase_t;

namespace triagens {
  namespace rest {
    class ApplicationDispatcher;
  }

  namespace arango {
    class ApplicationV8;

// -----------------------------------------------------------------------------
// --SECTION--                                                   HeartbeatThread
// -----------------------------------------------------------------------------

    class HeartbeatThread : public basics::Thread {

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      private:
        HeartbeatThread (HeartbeatThread const&);
        HeartbeatThread& operator= (HeartbeatThread const&);

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a heartbeat thread
////////////////////////////////////////////////////////////////////////////////

        HeartbeatThread (TRI_server_t*,
                         triagens::rest::ApplicationDispatcher*,
                         ApplicationV8*,
                         uint64_t,
                         uint64_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a heartbeat thread
////////////////////////////////////////////////////////////////////////////////

        ~HeartbeatThread ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes the heartbeat
////////////////////////////////////////////////////////////////////////////////

        bool init ();

////////////////////////////////////////////////////////////////////////////////
/// @brief stops the heartbeat
////////////////////////////////////////////////////////////////////////////////

        void stop () {
          if (_stop > 0) {
            return;
          }

          LOG_TRACE("stopping heartbeat thread");

          _stop = 1;
          _condition.signal();

          while (_stop != 2) {
            usleep(1000);
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the thread is ready
////////////////////////////////////////////////////////////////////////////////

        bool isReady ();

////////////////////////////////////////////////////////////////////////////////
/// @brief set the thread status to ready
////////////////////////////////////////////////////////////////////////////////

        void setReady ();

////////////////////////////////////////////////////////////////////////////////
/// @brief decrement the counter for a dispatched job
////////////////////////////////////////////////////////////////////////////////

        void removeDispatchedJob ();

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the thread has run at least once.
/// this is used on the coordinator only
////////////////////////////////////////////////////////////////////////////////

        static bool hasRunOnce () {
          return (HasRunOnce == 1); 
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                    Thread methods
// -----------------------------------------------------------------------------

      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief heartbeat main loop
////////////////////////////////////////////////////////////////////////////////

        void run ();

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief handles a plan change, coordinator case
////////////////////////////////////////////////////////////////////////////////

        bool handlePlanChangeCoordinator (uint64_t,
                                          uint64_t&);

////////////////////////////////////////////////////////////////////////////////
/// @brief handles a plan change, DBServer case
////////////////////////////////////////////////////////////////////////////////

        bool handlePlanChangeDBServer (uint64_t,
                                       uint64_t&);

////////////////////////////////////////////////////////////////////////////////
/// @brief handles a state change
////////////////////////////////////////////////////////////////////////////////

        bool handleStateChange (AgencyCommResult&,
                                uint64_t&);

////////////////////////////////////////////////////////////////////////////////
/// @brief fetch the last value of Sync/Commands/my-id from the agency
////////////////////////////////////////////////////////////////////////////////

        uint64_t getLastCommandIndex ();

////////////////////////////////////////////////////////////////////////////////
/// @brief sends the current server's state to the agency
////////////////////////////////////////////////////////////////////////////////

        bool sendState ();

////////////////////////////////////////////////////////////////////////////////
/// @brief fetch users for a database (run on coordinator only)
////////////////////////////////////////////////////////////////////////////////

        bool fetchUsers (TRI_vocbase_t*);

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief server
////////////////////////////////////////////////////////////////////////////////

         TRI_server_t* _server;

////////////////////////////////////////////////////////////////////////////////
/// @brief Job dispatcher
////////////////////////////////////////////////////////////////////////////////

        triagens::rest::ApplicationDispatcher* _dispatcher;

////////////////////////////////////////////////////////////////////////////////
/// @brief v8 dispatcher
////////////////////////////////////////////////////////////////////////////////

        ApplicationV8* _applicationV8;

////////////////////////////////////////////////////////////////////////////////
/// @brief status lock
////////////////////////////////////////////////////////////////////////////////

        triagens::basics::Mutex _statusLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief AgencyComm instance
////////////////////////////////////////////////////////////////////////////////

        AgencyComm _agency;

////////////////////////////////////////////////////////////////////////////////
/// @brief condition variable for heartbeat
////////////////////////////////////////////////////////////////////////////////

        triagens::basics::ConditionVariable _condition;

////////////////////////////////////////////////////////////////////////////////
/// @brief users for these databases will be re-fetched the next time the
/// heartbeat thread runs
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_vocbase_t*> _refetchUsers;

////////////////////////////////////////////////////////////////////////////////
/// @brief this server's id
////////////////////////////////////////////////////////////////////////////////

        std::string const _myId;

////////////////////////////////////////////////////////////////////////////////
/// @brief heartbeat interval
////////////////////////////////////////////////////////////////////////////////

        uint64_t _interval;

////////////////////////////////////////////////////////////////////////////////
/// @brief number of fails in a row before a warning is issued
////////////////////////////////////////////////////////////////////////////////

        uint64_t _maxFailsBeforeWarning;

////////////////////////////////////////////////////////////////////////////////
/// @brief current number of fails in a row
////////////////////////////////////////////////////////////////////////////////

        uint64_t _numFails;

////////////////////////////////////////////////////////////////////////////////
/// @brief current number of dispatched (pending) jobs
////////////////////////////////////////////////////////////////////////////////

        uint64_t _numDispatchedJobs;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the thread is ready
////////////////////////////////////////////////////////////////////////////////

        bool _ready;

////////////////////////////////////////////////////////////////////////////////
/// @brief stop flag
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t _stop;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the heartbeat thread has run at least once
/// this is used on the coordinator only
////////////////////////////////////////////////////////////////////////////////

        static volatile sig_atomic_t HasRunOnce;

    };

  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
