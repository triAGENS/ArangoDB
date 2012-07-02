////////////////////////////////////////////////////////////////////////////////
/// @brief general server job
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2012 triAGENS GmbH, Cologne, Germany
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
/// @author Copyright 2009-2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_GENERAL_SERVER_GENERAL_SERVER_JOB_H
#define TRIAGENS_GENERAL_SERVER_GENERAL_SERVER_JOB_H 1

#include "Dispatcher/Job.h"

#include "Logger/Logger.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/Mutex.h"
#include "Rest/Handler.h"
#include "Scheduler/AsyncTask.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

namespace triagens {
  namespace rest {
    class Dispatcher;
    class Scheduler;

// -----------------------------------------------------------------------------
// --SECTION--                                            class GeneralServerJob
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup GeneralServer
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief general server job
////////////////////////////////////////////////////////////////////////////////

    template<typename S, typename H>
    class GeneralServerJob : public Job {
      private:
        GeneralServerJob (GeneralServerJob const&);
        GeneralServerJob& operator= (GeneralServerJob const&);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup GeneralServer
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a new server job
////////////////////////////////////////////////////////////////////////////////

        GeneralServerJob (S* server, Dispatcher* dispatcher, H* handler)
          : Job("HttpServerJob"),
            _server(server),
            _dispatcher(dispatcher),
            _handler(handler),
            _shutdown(0),
            _abandon(0) {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructs a server job
////////////////////////////////////////////////////////////////////////////////

        ~GeneralServerJob () {
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup GeneralServer
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief abandon job
////////////////////////////////////////////////////////////////////////////////

        void abandon () {
          _abandon = 1;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the underlying handler
////////////////////////////////////////////////////////////////////////////////

        H* getHandler () const {
          return _handler;
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                       Job methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup GeneralServer
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        JobType type () {
          return _handler->type();
        }

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        string const& queue () {
          return _handler->queue();
        }

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void setDispatcherThread (DispatcherThread* thread) {
          _handler->setDispatcherThread(thread);
        }

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        status_e work () {
          LOGGER_TRACE << "beginning job " << static_cast<Job*>(this);

          if (_shutdown != 0) {
            return Job::JOB_DONE;
          }

          Handler::status_e status = _handler->execute();

          LOGGER_TRACE << "finished job " << static_cast<Job*>(this) << " with status " << status;

          switch (status) {
            case Handler::HANDLER_DONE:    return Job::JOB_DONE;
            case Handler::HANDLER_DETACH:  return Job::JOB_DETACH;
            case Handler::HANDLER_REQUEUE: return Job::JOB_REQUEUE;
            case Handler::HANDLER_FAILED:  return Job::JOB_FAILED;
          }

          return Job::JOB_FAILED;
        }

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void cleanup () {
          if (_abandon == 0) {
            _server->jobDone(this);
          }

          delete this;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief shuts down the execution and deletes everything
////////////////////////////////////////////////////////////////////////////////

        bool beginShutdown () {
          LOGGER_TRACE << "shutdown job " << static_cast<Job*>(this);

          _shutdown = 1;
          return true;
        }

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void handleError (basics::TriagensError const& ex) {
          _handler->handleError(ex);
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup GeneralServer
/// @{
////////////////////////////////////////////////////////////////////////////////

      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief general server
////////////////////////////////////////////////////////////////////////////////

        S* _server;

////////////////////////////////////////////////////////////////////////////////
/// @brief dispatcher
////////////////////////////////////////////////////////////////////////////////

        Dispatcher* _dispatcher;

////////////////////////////////////////////////////////////////////////////////
/// @brief handler
////////////////////////////////////////////////////////////////////////////////

        H* _handler;

////////////////////////////////////////////////////////////////////////////////
/// @brief shutdown in progress
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t _shutdown;

////////////////////////////////////////////////////////////////////////////////
/// @brief server is dead
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t _abandon;
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
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\)"
// End:
