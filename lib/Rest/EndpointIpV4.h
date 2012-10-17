////////////////////////////////////////////////////////////////////////////////
/// @brief connection endpoints
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
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_FYN_REST_ENDPOINT_IPV4_H
#define TRIAGENS_FYN_REST_ENDPOINT_IPV4_H 1

#include <Basics/Common.h>
#include <Basics/StringUtils.h>



// -----------------------------------------------------------------------------
// --SECTION--                                                          Endpoint
// -----------------------------------------------------------------------------

namespace triagens {
  namespace rest {

////////////////////////////////////////////////////////////////////////////////
/// @brief endpoint specification
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                      EndpointIpV4
// -----------------------------------------------------------------------------

    class EndpointIpV4 : public EndpointIp {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Rest
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an endpoint
////////////////////////////////////////////////////////////////////////////////

        EndpointIpV4 (const Endpoint::Type, 
                      const Endpoint::Protocol,
                      const Endpoint::Encryption,
                      const std::string&, 
                      const std::string&, 
                      const uint16_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an endpoint
////////////////////////////////////////////////////////////////////////////////
      
        ~EndpointIpV4 ();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Rest
/// @{
////////////////////////////////////////////////////////////////////////////////
    
      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief get endpoint domain
////////////////////////////////////////////////////////////////////////////////
        
        int getDomain () const {
          return AF_INET;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get host string for HTTP requests
////////////////////////////////////////////////////////////////////////////////

        string getHostString  () const {
          return _host + ':' + triagens::basics::StringUtils::itoa(_port);
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                      EndpointIpV6
// -----------------------------------------------------------------------------


  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
