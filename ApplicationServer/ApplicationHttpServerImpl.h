////////////////////////////////////////////////////////////////////////////////
/// @brief application server http server implementation
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

#ifndef TRIAGENS_FYN_APPLICATION_SERVER_APPLICATION_HTTP_SERVER_IMPL_H
#define TRIAGENS_FYN_APPLICATION_SERVER_APPLICATION_HTTP_SERVER_IMPL_H 1

#include <Rest/ApplicationHttpServer.h>

namespace triagens {
  namespace rest {
    class HttpServer;
    class HttpServerImpl;

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief application server scheduler implementation
    ////////////////////////////////////////////////////////////////////////////////

    class ApplicationHttpServerImpl : public ApplicationHttpServer {
      private:
        ApplicationHttpServerImpl (ApplicationHttpServerImpl const&);
        ApplicationHttpServerImpl& operator= (ApplicationHttpServerImpl const&);

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief constructor
        ////////////////////////////////////////////////////////////////////////////////

        ApplicationHttpServerImpl (ApplicationServer*);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief destructor
        ////////////////////////////////////////////////////////////////////////////////

        ~ApplicationHttpServerImpl ();

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        void setupOptions (map<string, basics::ProgramOptionsDescription>&);

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        bool parsePhase2 (basics::ProgramOptions&);

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        void showPortOptions (bool value) {
          showPort = value;
        }

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        AddressPort addPort (string const&);

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        HttpServer* buildServer (HttpHandlerFactory*);

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        HttpServer* buildServer (HttpHandlerFactory*, vector<AddressPort> const&);

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        HttpServer* buildServer (HttpServer*, HttpHandlerFactory*, vector<AddressPort> const&);

      protected:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief build an http server
        ////////////////////////////////////////////////////////////////////////////////

        HttpServerImpl* buildHttpServer (HttpServerImpl*, HttpHandlerFactory*, vector<AddressPort> const&);

      protected:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief application server
        ////////////////////////////////////////////////////////////////////////////////

        ApplicationServer* applicationServer;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief show port options
        ////////////////////////////////////////////////////////////////////////////////

        bool showPort;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief is keep-alive required to keep the connection open
        ////////////////////////////////////////////////////////////////////////////////

        bool requireKeepAlive;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief all constructed http servers
        ////////////////////////////////////////////////////////////////////////////////

        vector<HttpServer*> httpServers;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief all default ports
        ////////////////////////////////////////////////////////////////////////////////

        vector<string> httpPorts;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief all used addresses
        ////////////////////////////////////////////////////////////////////////////////

        vector<AddressPort> httpAddressPorts;
    };
  }
}

#endif
