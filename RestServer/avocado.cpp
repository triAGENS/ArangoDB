////////////////////////////////////////////////////////////////////////////////
/// @brief AvocadoDB server
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
/// @author Copyright 2010-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include <Basics/Common.h>

#include <Rest/Initialise.h>

#include "RestServer/AvocadoServer.h"

using namespace triagens;
using namespace triagens::basics;
using namespace triagens::rest;
using namespace triagens::avocado;

////////////////////////////////////////////////////////////////////////////////
/// @page StartStopTOC
///
/// <ol>
///  <li>Starting the Debug Shell</li>
///  <li>Starting the HTTP Server</li>
///  <li>Frequently Used Options</li>
/// </ol>
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @page StartStop Starting and Stopping the AvocadoDB
///
/// The AvocadoDB has two mode of operations: as server, where it will answer to
/// HTTP request, see @ref HttpInterface, and a debug shell, where you can
/// access the database directly. Using the debug shell always you to issue all
/// command normally available in actions and transactions, see @ref
/// AvocadoScript.
///
/// You should never start more than one server for the same database,
/// independent from the mode of operation.
///
/// <hr>
/// @copydoc StartStopTOC
///
/// @section StartStopShell Starting the Debug Shell
///
/// The following command starts a debug shell. See below for a list of
/// frequently used options, see @ref CommandLine "here" for a complete list.
///
/// @verbinclude start1
///
/// @section StartStopServer Starting the HTTP Server
///
/// The following command starts the AvocadoDB in server mode. You will be able
/// to access the server using HTTP request on port 8529. See below for a list
/// of frequently used options, see @ref CommandLine "here" for a complete list.
///
/// @verbinclude start2
///
/// @section StartStopOptions Frequently Used Options
///
/// The following main command-line options are available.
///
/// @CMDOPT{@CA{database-directory}}
///
/// Uses the @CA{database-directory} as base directory. There is an alternative
/// version available for use in configuration files, see @ref
/// CommandLineAvocado "here".
///
/// @copydetails triagens::rest::ApplicationServerImpl::options
///
/// @CMDOPT{--log @CA{level}}
///
/// Allows the user to choose the level of information which is logged by the
/// server. The arg is specified as a string and can be one of the following
/// values: fatal, error, warning, info, debug, trace.  For more information see
/// @ref CommandLineLogging "here".
///
/// @copydetails triagens::avocado::AvocadoServer::_httpPort
///
/// @CMDOPT{--shell}
///
/// Opens a debug shell instead of starting the HTTP server.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @page CommandLineTOC
///
/// <ol>
///  <li>configuration</li>
///  <li>daemon</li>
///  <li>gid</li>
///  <li>help</li>
///  <li>pid-file</li>
///  <li>uid</li>
///  <li>version</li>
/// </ol>
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @page CommandLine Command-Line Options
///
/// @copydoc CommandLineTOC
///
/// @section GeneralOptions General Options
///
/// @copydetails triagens::rest::ApplicationServerImpl::initFile
///
/// @CMDOPT{--daemon}
///
/// Runs the server as a daemon (as a background process). This parameter can
/// only be set if the pid (process id) file is specified. That is, unless a
/// value to the parameter pid-file is given, then the server will report an
/// error and exit.
///
/// @copydetails triagens::rest::ApplicationServerImpl::gid
///
/// @copydetails triagens::rest::ApplicationServerImpl::options
///
/// @copydetails triagens::rest::AnyServer::_pidFile
///
/// @CMDOPT{--show-io-backends}
///
/// If this option is specified, then the server will list available backends
/// and exit. This option is useful only when used in conjunction with the
/// option scheduler.backend. An integer is returned (which is platform
/// dependent) which indicates available backends on your platform. See libev
/// for further details and for the meaning of the integer returned. This
/// describes the allowed integers for @CODE{scheduler.backend}, see
/// @ref CommandLineScheduler "here" for details.
///
/// @CMDOPT{--supervisor}
///
/// Executes the server in supervisor mode. In the event that the server
/// unexpectedly terminates due to an internal error, the supervisor will
/// automatically restart the server. Setting this flag automatically implies
/// that the server will run as a daemon. Note that, as with the daemon flag,
/// this flag requires that the pid-file parameter will set.
///
/// @copydetails triagens::rest::ApplicationServerImpl::uid
///
/// @copydetails triagens::rest::ApplicationServerImpl::version
///
/// <hr>
/// Next steps:
///
///  - @ref CommandLineAvocado "Command-Line Options for the AvocadoDB"
///  - @ref CommandLineScheduler "Command-Line Options for Communication"
///  - @ref CommandLineLogging "Command-Line Options for Logging"
///  - @ref CommandLineRandom "Command-Line Options for Random Numbers"
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @page CommandLineAvocadoTOC
///
/// <ol>
///  <li>database.directory</li>
///  <li>server.admin-port</li>
///  <li>server.http-port</li>
/// </ol>
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @page CommandLineAvocado Command-Line Options for the AvocadoDB
///
/// @copydoc CommandLineAvocadoTOC
/// <hr>
///
/// @copydetails triagens::avocado::AvocadoServer::_databasePath
///
/// @copydetails triagens::avocado::AvocadoServer::_adminPort
///
/// @copydetails triagens::avocado::AvocadoServer::_httpPort
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup AvocadoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an application server
////////////////////////////////////////////////////////////////////////////////

int main (int argc, char* argv[]) {
  TRIAGENS_REST_INITIALISE;
  TRI_InitialiseVocBase();

  // create and start a AvocadoDB server
  AvocadoServer server(argc, argv);

  int res = server.start();

  // shutdown
  TRI_ShutdownVocBase();
  TRIAGENS_REST_SHUTDOWN;

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
