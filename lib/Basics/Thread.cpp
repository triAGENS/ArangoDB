////////////////////////////////////////////////////////////////////////////////
/// @brief Thread
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

#ifdef _WIN32
#include "BasicsC/win-utils.h"
#endif

#include "Thread.h"

#include <errno.h>
#include <signal.h>

#include "Basics/ConditionLocker.h"
#include "BasicsC/logging.h"


using namespace triagens::basics;

// -----------------------------------------------------------------------------
// --SECTION--                                            static private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief static started with access to the private variables
////////////////////////////////////////////////////////////////////////////////

void Thread::startThread (void* arg) {
  Thread * ptr = (Thread *) arg;

  ptr->runMe();
  ptr->cleanup();
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                             static public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the process id
////////////////////////////////////////////////////////////////////////////////

TRI_pid_t Thread::currentProcessId () {
  return TRI_CurrentProcessId();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the thread process id
////////////////////////////////////////////////////////////////////////////////

TRI_pid_t Thread::currentThreadProcessId () {
  return TRI_CurrentThreadProcessId();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the thread id
////////////////////////////////////////////////////////////////////////////////

TRI_tid_t Thread::currentThreadId () {
  return TRI_CurrentThreadId();
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a thread
////////////////////////////////////////////////////////////////////////////////

Thread::Thread (std::string const& name)
  : _name(name),
    _asynchronousCancelation(false),
    _thread(),
    _finishedCondition(0),
    _started(0),
    _running(0) {
  TRI_InitThread(&_thread);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes the thread
////////////////////////////////////////////////////////////////////////////////

Thread::~Thread () {
  if (_running != 0) {
    LOG_WARNING("forcefully shutting down thread '%s'", _name.c_str());
    TRI_StopThread(&_thread);
  }

  TRI_DetachThread(&_thread);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for running
////////////////////////////////////////////////////////////////////////////////

bool Thread::isRunning () {
  return _running != 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a thread identifier
////////////////////////////////////////////////////////////////////////////////

TRI_tid_t Thread::threadId () {
  return (TRI_tid_t) _thread;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief starts the thread
////////////////////////////////////////////////////////////////////////////////

bool Thread::start (ConditionVariable * finishedCondition) {
  _finishedCondition = finishedCondition;

  if (_started != 0) {
    LOG_FATAL_AND_EXIT("called started on an already started thread");
  }

  _started = 1;

  string text = "[" + _name + "]";
  bool ok = TRI_StartThread(&_thread, text.c_str(), &startThread, this);

  if (! ok) {
    LOG_ERROR("could not start thread '%s': %s", _name.c_str(), strerror(errno));
  }

  return ok;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stops the thread
////////////////////////////////////////////////////////////////////////////////

void Thread::stop () {
  if (_running != 0) {
    LOG_TRACE("trying to cancel (aka stop) the thread '%s'", _name.c_str());
    TRI_StopThread(&_thread);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief joins the thread
////////////////////////////////////////////////////////////////////////////////

void Thread::join () {
  TRI_JoinThread(&_thread);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stops and joins the thread
////////////////////////////////////////////////////////////////////////////////

void Thread::shutdown () {
  size_t const MAX_TRIES = 10;
  size_t const WAIT = 10000;

  for (size_t i = 0;  i < MAX_TRIES;  ++i) {
    if (_running == 0) {
      break;
    }

    usleep(WAIT);
  }

  if (_running != 0) {
    LOG_TRACE("trying to cancel (aka stop) the thread '%s'", _name.c_str());
    TRI_StopThread(&_thread);
  }

  TRI_JoinThread(&_thread);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief send signal to thread
////////////////////////////////////////////////////////////////////////////////

void Thread::sendSignal (int signal) {
  if (_running != 0) {
    TRI_SignalThread(&_thread, signal);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 protected methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief allows asynchrounous cancelation
////////////////////////////////////////////////////////////////////////////////

void Thread::allowAsynchronousCancelation () {
  if (_started) {
    if (_running) {
      if (TRI_IsSelfThread(&_thread)) {
        LOG_DEBUG("set asynchronous cancelation for thread '%s'", _name.c_str());
        TRI_AllowCancelation();
      }
      else {
        LOG_ERROR("cannot change cancelation type of an already running thread from the outside");
      }
    }
    else {
      LOG_WARNING("thread has already stopped, it is useless to change the cancelation type");
    }
  }
  else {
    _asynchronousCancelation = true;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Threading
/// @{
////////////////////////////////////////////////////////////////////////////////

void Thread::runMe () {
  if (_asynchronousCancelation) {
    LOG_DEBUG("set asynchronous cancelation for thread '%s'", _name.c_str());
    TRI_AllowCancelation();
  }

  _running = 1;

  try {
    run();
  }
  catch (...) {
    _running = 0;
    LOG_WARNING("exception caught in thread '%s'", _name.c_str());
    throw;
  }

  _running = 0;

  if (_finishedCondition != 0) {
    CONDITION_LOCKER(locker, *_finishedCondition);
    locker.broadcast();
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
