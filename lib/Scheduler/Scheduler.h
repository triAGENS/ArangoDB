////////////////////////////////////////////////////////////////////////////////
/// @brief input-output scheduler
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// @author Achim Brandt
/// @author Copyright 2008-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_SCHEDULER_SCHEDULER_H
#define TRIAGENS_SCHEDULER_SCHEDULER_H 1

#include "BasicsC/socket-utils.h"
#include "Scheduler/TaskManager.h"

#include "Basics/Mutex.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

namespace triagens {
  namespace basics {
    class ConditionVariable;
  }

  namespace rest {
    class SchedulerThread;

// -----------------------------------------------------------------------------
// --SECTION--                                                   class Scheduler
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief input-output scheduler
////////////////////////////////////////////////////////////////////////////////

   class Scheduler : private TaskManager {
      private:
        Scheduler (Scheduler const&);
        Scheduler& operator= (Scheduler const&);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
///
/// If the number of threads is one, then the scheduler is single-threaded.
/// In this case the only methods, which can be called from a different thread
/// are beginShutdown, isShutdownInProgress, and isRunning. The method
/// registerTask must be called before the Scheduler is started or from
/// within the Scheduler thread.
///
/// If the number of threads is greater than one, then the scheduler is
/// multi-threaded. In this case the method registerTask can be called from
/// threads other than the scheduler.
////////////////////////////////////////////////////////////////////////////////

        Scheduler (size_t nrThreads);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        virtual ~Scheduler ();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief starts scheduler, keeps running
///
/// The functions returns true, if the scheduler has been started. In this
/// case the condition variable is signal as soon as at least one of the
/// scheduler threads stops.
////////////////////////////////////////////////////////////////////////////////

        bool start (basics::ConditionVariable*);

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if the scheduler threads are up and running
////////////////////////////////////////////////////////////////////////////////

        bool isStarted ();

////////////////////////////////////////////////////////////////////////////////
/// @brief opens the scheduler for business
////////////////////////////////////////////////////////////////////////////////

        bool open ();

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if scheduler is still running
////////////////////////////////////////////////////////////////////////////////

        bool isRunning ();

////////////////////////////////////////////////////////////////////////////////
/// @brief starts shutdown sequence
////////////////////////////////////////////////////////////////////////////////

        void beginShutdown ();

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if scheduler is shuting down
////////////////////////////////////////////////////////////////////////////////

        bool isShutdownInProgress ();

////////////////////////////////////////////////////////////////////////////////
/// @brief shuts down the scheduler
////////////////////////////////////////////////////////////////////////////////

        void shutdown ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get all user tasks
////////////////////////////////////////////////////////////////////////////////

        std::vector<std::pair<uint64_t, std::string> > getUserTasks ();

////////////////////////////////////////////////////////////////////////////////
/// @brief cancel a user task by id
////////////////////////////////////////////////////////////////////////////////

        bool unregisterUserTask (uint64_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief registers a new task
////////////////////////////////////////////////////////////////////////////////

        bool registerTask (Task*);

////////////////////////////////////////////////////////////////////////////////
/// @brief unregisters a task
///
/// Note that this method is called by the task itself when cleanupTask is
/// executed. If a Task failed in setupTask, it must not call unregisterTask.
////////////////////////////////////////////////////////////////////////////////

        void unregisterTask (Task*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys task
///
/// Even if a Task failed in setupTask, it can still call destroyTask. The
/// methods will delete the task.
////////////////////////////////////////////////////////////////////////////////

        void destroyTask (Task*);

////////////////////////////////////////////////////////////////////////////////
/// @brief called to display current status
////////////////////////////////////////////////////////////////////////////////

        void reportStatus ();

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the scheduler is active
////////////////////////////////////////////////////////////////////////////////
        
        bool isActive () const {
          return (bool) _active;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the scheduler activity
////////////////////////////////////////////////////////////////////////////////
        
        void setActive (bool value) {
          _active = value ? 1 : 0;
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                            virtual public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief main event loop
////////////////////////////////////////////////////////////////////////////////

        virtual void eventLoop (EventLoop) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief wakes up an event loop
////////////////////////////////////////////////////////////////////////////////

        virtual void wakeupLoop (EventLoop) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to register a socket descriptor event
////////////////////////////////////////////////////////////////////////////////

        virtual EventToken installSocketEvent (EventLoop, EventType, Task*, TRI_socket_t) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief re-starts the socket events
////////////////////////////////////////////////////////////////////////////////

        virtual void startSocketEvents (EventToken) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief stops the socket events
////////////////////////////////////////////////////////////////////////////////

        virtual void stopSocketEvents (EventToken) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to register an asynchronous event
////////////////////////////////////////////////////////////////////////////////

        virtual EventToken installAsyncEvent (EventLoop, Task*) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief sends an asynchronous event
////////////////////////////////////////////////////////////////////////////////

        virtual void sendAsync (EventToken) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to register a timer event
////////////////////////////////////////////////////////////////////////////////

        virtual EventToken installTimerEvent (EventLoop, Task*, double timeout) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief clears a timer without removing it
////////////////////////////////////////////////////////////////////////////////

        virtual void clearTimer (EventToken) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief rearms a timer
////////////////////////////////////////////////////////////////////////////////

        virtual void rearmTimer (EventToken, double timeout) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to register a periodic event
////////////////////////////////////////////////////////////////////////////////

        virtual EventToken installPeriodicEvent (EventLoop, Task*, double offset, double interval) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief rearms a periodic timer
////////////////////////////////////////////////////////////////////////////////

        virtual void rearmPeriodic (EventToken, double offset, double timeout) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to register a signal event
////////////////////////////////////////////////////////////////////////////////

        virtual EventToken installSignalEvent (EventLoop, Task*, int signal) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief called to unregister an event handler
////////////////////////////////////////////////////////////////////////////////

        virtual void uninstallEvent (EventToken) = 0;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

     protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief number of scheduler threads
////////////////////////////////////////////////////////////////////////////////

        size_t nrThreads;

////////////////////////////////////////////////////////////////////////////////
/// @brief scheduler threads
////////////////////////////////////////////////////////////////////////////////

        SchedulerThread** threads;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                            static private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief inialises the signal handlers for the scheduler
////////////////////////////////////////////////////////////////////////////////

        static void initialiseSignalHandlers ();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Scheduler
/// @{
////////////////////////////////////////////////////////////////////////////////

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief true if scheduler is shutting down
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t stopping;

////////////////////////////////////////////////////////////////////////////////
/// @brief true if scheduler is multi-threaded
////////////////////////////////////////////////////////////////////////////////

        bool multiThreading;

////////////////////////////////////////////////////////////////////////////////
/// @brief round-robin for event loops
////////////////////////////////////////////////////////////////////////////////

        size_t nextLoop;

////////////////////////////////////////////////////////////////////////////////
/// @brief lock for scheduler threads
////////////////////////////////////////////////////////////////////////////////

        basics::Mutex schedulerLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief tasks to thread
////////////////////////////////////////////////////////////////////////////////

        map<Task*, SchedulerThread*> task2thread;

////////////////////////////////////////////////////////////////////////////////
/// @brief active tasks
////////////////////////////////////////////////////////////////////////////////

        set<Task*> taskRegistered;

////////////////////////////////////////////////////////////////////////////////
/// @brief scheduler activity flag
////////////////////////////////////////////////////////////////////////////////

        bool _active;
    };
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
