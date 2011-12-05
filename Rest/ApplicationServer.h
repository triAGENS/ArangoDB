////////////////////////////////////////////////////////////////////////////////
/// @brief application server
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
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
/// @author Copyright 2009-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_FYN_REST_APPLICATION_SERVER_H
#define TRIAGENS_FYN_REST_APPLICATION_SERVER_H 1

#include <Basics/Common.h>

namespace triagens {
  namespace basics {
    class ProgramOptionsDescription;
    class ProgramOptions;
  }

  namespace rest {
    class ApplicationFeature;
    class Scheduler;
    class SignalTask;

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief application server
    ////////////////////////////////////////////////////////////////////////////////

    class ApplicationServer {
      private:
        ApplicationServer (const ApplicationServer&);
        ApplicationServer& operator= (const ApplicationServer&);

      public:
        static string const OPTIONS_CMDLINE;
        static string const OPTIONS_HIDDEN;
        static string const OPTIONS_LIMITS;
        static string const OPTIONS_LOGGER;
        static string const OPTIONS_SERVER;
        static string const OPTIONS_RECOVERY_REPLICATION;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief constructs a new skeleton
        ////////////////////////////////////////////////////////////////////////////////

        static ApplicationServer* create (string const& description, string const& version);

      protected:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief constructor
        ////////////////////////////////////////////////////////////////////////////////

        ApplicationServer () {
        }

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief destructor
        ////////////////////////////////////////////////////////////////////////////////

        virtual ~ApplicationServer () {
        }

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief adds a new feature
        ////////////////////////////////////////////////////////////////////////////////

        virtual void addFeature (ApplicationFeature*) = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief sets the name of the system config file
        ////////////////////////////////////////////////////////////////////////////////

        virtual void setSystemConfigFile (string const& name) = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief sets the name of the user config file
        ////////////////////////////////////////////////////////////////////////////////

        virtual void setUserConfigFile (string const& name) = 0;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief allows a multi scheduler to be build
        ////////////////////////////////////////////////////////////////////////////////

        virtual void allowMultiScheduler (bool value = true) = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the scheduler
        ////////////////////////////////////////////////////////////////////////////////

        virtual Scheduler* scheduler () const = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief builds the scheduler
        ////////////////////////////////////////////////////////////////////////////////

        virtual void buildScheduler () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief builds the scheduler reporter
        ////////////////////////////////////////////////////////////////////////////////

        virtual void buildSchedulerReporter () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief quits on control-c signal
        ////////////////////////////////////////////////////////////////////////////////

        virtual void buildControlCHandler () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief installs a signal handler
        ////////////////////////////////////////////////////////////////////////////////

        virtual void installSignalHandler (SignalTask*) = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns true, if address reuse is allowed
        ////////////////////////////////////////////////////////////////////////////////

        virtual bool addressReuseAllowed () = 0;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the command line options
        ////////////////////////////////////////////////////////////////////////////////

        virtual basics::ProgramOptions& programOptions () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the command line arguments
        ////////////////////////////////////////////////////////////////////////////////

        virtual vector<string> programArguments () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief parses the arguments
        ////////////////////////////////////////////////////////////////////////////////

        bool parse (int argc, char* argv[]);

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief parses the arguments
        ////////////////////////////////////////////////////////////////////////////////

        virtual bool parse (int argc, char* argv[], map<string, basics::ProgramOptionsDescription>) = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief starts the scheduler
        ////////////////////////////////////////////////////////////////////////////////

        virtual void start () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief waits for shutdown
        ////////////////////////////////////////////////////////////////////////////////

        virtual void wait () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief begins shutdown sequence
        ////////////////////////////////////////////////////////////////////////////////

        virtual void beginShutdown () = 0;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief shut downs everything
        ////////////////////////////////////////////////////////////////////////////////

        virtual void shutdown () = 0;
    };
  }
}

#endif
