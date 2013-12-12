////////////////////////////////////////////////////////////////////////////////
/// @brief communication with agency node(s)
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Copyright 2013, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_CLUSTER_AGENCY_COMM_H
#define TRIAGENS_CLUSTER_AGENCY_COMM_H 1

#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "Rest/HttpRequest.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TRI_json_s;

namespace triagens {
  namespace httpclient {
    class GeneralClientConnection;
  }
  
  namespace rest {
    class Endpoint;
  }

  namespace arango {

// -----------------------------------------------------------------------------
// --SECTION--                                                    AgencyEndpoint
// -----------------------------------------------------------------------------

    struct AgencyEndpoint {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an agency endpoint
////////////////////////////////////////////////////////////////////////////////

      AgencyEndpoint (triagens::rest::Endpoint*,
                      triagens::httpclient::GeneralClientConnection*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an agency endpoint
////////////////////////////////////////////////////////////////////////////////
      
      ~AgencyEndpoint (); 

// -----------------------------------------------------------------------------
// --SECTION--                                                  public variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief the endpoint 
////////////////////////////////////////////////////////////////////////////////

      triagens::rest::Endpoint* _endpoint;

////////////////////////////////////////////////////////////////////////////////
/// @brief the connection
////////////////////////////////////////////////////////////////////////////////

      triagens::httpclient::GeneralClientConnection* _connection;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the endpoint is busy
////////////////////////////////////////////////////////////////////////////////

      bool _busy;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                           AgencyConnectionOptions
// -----------------------------------------------------------------------------

    struct AgencyConnectionOptions {
      double _connectTimeout;
      double _requestTimeout;
      size_t _connectRetries;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  AgencyCommResult
// -----------------------------------------------------------------------------

    struct AgencyCommResult {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a communication result
////////////////////////////////////////////////////////////////////////////////

      AgencyCommResult ();

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a communication result
////////////////////////////////////////////////////////////////////////////////

      ~AgencyCommResult ();

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief returns whether the last request was successful
////////////////////////////////////////////////////////////////////////////////

      inline bool successful () const {
        return (_statusCode >= 200 && _statusCode <= 299);
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief extract the error message from the result
/// if there is no error, an empty string will be returned
////////////////////////////////////////////////////////////////////////////////

      std::string errorMessage () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief extract the error details from the result
/// if there is no error, an empty string will be returned
////////////////////////////////////////////////////////////////////////////////

      std::string errorDetails () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return the location header (might be empty)
////////////////////////////////////////////////////////////////////////////////
      
      const std::string location () const {
        return _location;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief recursively flatten the JSON response into a map
////////////////////////////////////////////////////////////////////////////////

      bool processJsonNode (struct TRI_json_s const*,
                            std::map<std::string, std::string>&,
                            std::string const&,
                            bool) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief turn a result into a map
////////////////////////////////////////////////////////////////////////////////

      bool flattenJson (std::map<std::string, std::string>&,
                        std::string const&,
                        bool) const; 

// -----------------------------------------------------------------------------
// --SECTION--                                                  public variables
// -----------------------------------------------------------------------------
  
    public:

      std::string _location;
      std::string _message;
      std::string _body;
      uint64_t    _index;
      int         _statusCode;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                        AgencyComm
// -----------------------------------------------------------------------------

    class AgencyComm {
      friend AgencyCommResult;

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a communication channel
////////////////////////////////////////////////////////////////////////////////

        AgencyComm (bool = true);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a communication channel
////////////////////////////////////////////////////////////////////////////////

        ~AgencyComm ();

// -----------------------------------------------------------------------------
// --SECTION--                                             public static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief cleans up all connections
////////////////////////////////////////////////////////////////////////////////

        static void cleanup (); 

////////////////////////////////////////////////////////////////////////////////
/// @brief tries to establish a communication channel
////////////////////////////////////////////////////////////////////////////////

        static bool tryConnect ();

////////////////////////////////////////////////////////////////////////////////
/// @brief disconnects all communication channels
////////////////////////////////////////////////////////////////////////////////
        
        static void disconnect ();

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an endpoint to the agents list
////////////////////////////////////////////////////////////////////////////////

        static bool addEndpoint (std::string const&,
                                 bool = false);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an endpoint from the agents list
////////////////////////////////////////////////////////////////////////////////

        static bool removeEndpoint (std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if an endpoint is present
////////////////////////////////////////////////////////////////////////////////

        static bool hasEndpoint (std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a stringified version of the endpoints
////////////////////////////////////////////////////////////////////////////////

        static const std::vector<std::string> getEndpoints ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get a stringified version of the endpoints
////////////////////////////////////////////////////////////////////////////////

        static const std::string getEndpointsString ();

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the global prefix for all operations
////////////////////////////////////////////////////////////////////////////////
        
        static void setPrefix (std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the global prefix for all operations
////////////////////////////////////////////////////////////////////////////////
        
        static std::string prefix ();

////////////////////////////////////////////////////////////////////////////////
/// @brief generate a timestamp
////////////////////////////////////////////////////////////////////////////////

        static std::string generateStamp ();

// -----------------------------------------------------------------------------
// --SECTION--                                            private static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new agency endpoint
////////////////////////////////////////////////////////////////////////////////

        static AgencyEndpoint* createAgencyEndpoint (std::string const&);

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief gets the backend version
////////////////////////////////////////////////////////////////////////////////

        std::string getVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a directory in the backend
////////////////////////////////////////////////////////////////////////////////
        
        AgencyCommResult createDirectory (std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets a value in the back end
////////////////////////////////////////////////////////////////////////////////

        AgencyCommResult setValue (std::string const&, 
                                   std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief gets one or multiple values from the back end
////////////////////////////////////////////////////////////////////////////////
        
        AgencyCommResult getValues (std::string const&, 
                                    bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes one or multiple values from the back end
////////////////////////////////////////////////////////////////////////////////
        
        AgencyCommResult removeValues (std::string const&,  
                                       bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief compares and swaps a single value in the backend
/// the CAS condition is whether or not a previous value existed for the key
////////////////////////////////////////////////////////////////////////////////

        AgencyCommResult casValue (std::string const&,
                                   std::string const&,
                                   bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief compares and swaps a single value in the back end
/// the CAS condition is whether or not the previous value for the key was
/// identical to `oldValue`
////////////////////////////////////////////////////////////////////////////////
        
        AgencyCommResult casValue (std::string const&, 
                                   std::string const&, 
                                   std::string const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief blocks on a change of a single value in the back end
////////////////////////////////////////////////////////////////////////////////
        
        AgencyCommResult watchValue (std::string const&, 
                                     uint64_t,
                                     double);

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------
      
      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief pop an endpoint from the queue
////////////////////////////////////////////////////////////////////////////////
    
        AgencyEndpoint* popEndpoint ();

////////////////////////////////////////////////////////////////////////////////
/// @brief reinsert an endpoint into the queue
////////////////////////////////////////////////////////////////////////////////
    
        void requeueEndpoint (AgencyEndpoint*,
                              bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief construct a URL
////////////////////////////////////////////////////////////////////////////////

        std::string buildUrl (std::string const&) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief sends an HTTP request to the agency, handling failover 
////////////////////////////////////////////////////////////////////////////////
    
        bool sendWithFailover (triagens::rest::HttpRequest::HttpRequestType,
                               double,
                               AgencyCommResult&,
                               std::string const&, 
                               std::string const&,
                               bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief sends data to the URL 
////////////////////////////////////////////////////////////////////////////////
    
        bool send (triagens::httpclient::GeneralClientConnection*,
                   triagens::rest::HttpRequest::HttpRequestType,
                   double,
                   AgencyCommResult&,
                   std::string const&, 
                   std::string const&);

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief automatically add unknown endpoints if redirected to by agency?
////////////////////////////////////////////////////////////////////////////////

        bool _addNewEndpoints;

// -----------------------------------------------------------------------------
// --SECTION--                                          private static variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the static global URL prefix
////////////////////////////////////////////////////////////////////////////////

        static const std::string AGENCY_URL_PREFIX;

////////////////////////////////////////////////////////////////////////////////
/// @brief the (variable) global prefix
////////////////////////////////////////////////////////////////////////////////

        static std::string _globalPrefix;

////////////////////////////////////////////////////////////////////////////////
/// @brief endpoints lock
////////////////////////////////////////////////////////////////////////////////

        static triagens::basics::ReadWriteLock _globalLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief all endpoints
////////////////////////////////////////////////////////////////////////////////
  
        static std::list<AgencyEndpoint*> _globalEndpoints;

////////////////////////////////////////////////////////////////////////////////
/// @brief global connection options
////////////////////////////////////////////////////////////////////////////////

        static AgencyConnectionOptions _globalConnectionOptions;

    };
  }
}


#ifdef __cplusplus
}
#endif

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

