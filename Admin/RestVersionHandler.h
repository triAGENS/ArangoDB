////////////////////////////////////////////////////////////////////////////////
/// @brief version request handler
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
/// @author Achim Brandt
/// @author Copyright 2010-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_BORNHOLM_ADMIN_REST_VERSION_HANDLER_H
#define TRIAGENS_BORNHOLM_ADMIN_REST_VERSION_HANDLER_H 1

#include <Admin/RestBaseHandler.h>

#include <Rest/HttpResponse.h>

// -----------------------------------------------------------------------------
// --SECTION--                                                RestVersionHandler
// -----------------------------------------------------------------------------

namespace triagens {
  namespace admin {

////////////////////////////////////////////////////////////////////////////////
/// @brief version request handler
////////////////////////////////////////////////////////////////////////////////

    class RestVersionHandler : public RestBaseHandler {

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        RestVersionHandler (rest::HttpRequest* request, pair<string,string> const* data);

// -----------------------------------------------------------------------------
// --SECTION--                                                   Handler methods
// -----------------------------------------------------------------------------

      public:


////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool isDirect ();


////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        status_e execute ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief server name
////////////////////////////////////////////////////////////////////////////////

        string _name;

////////////////////////////////////////////////////////////////////////////////
/// @brief version number
////////////////////////////////////////////////////////////////////////////////

        string _version;
    };
  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
