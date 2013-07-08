////////////////////////////////////////////////////////////////////////////////
/// @brief json helper functions
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
/// @author Jan Steemann
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_BASICS_JSON_HELPER_H
#define TRIAGENS_BASICS_JSON_HELPER_H 1

#include "Basics/Common.h"

struct TRI_json_s;

namespace triagens {
  namespace basics {

// -----------------------------------------------------------------------------
// --SECTION--                                                  class JsonHelper
// -----------------------------------------------------------------------------

    class JsonHelper {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      private:

        JsonHelper ();
        ~JsonHelper ();
        
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                             public static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////
      
      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief stringify json
////////////////////////////////////////////////////////////////////////////////
        
        static std::string toString (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true for arrays
////////////////////////////////////////////////////////////////////////////////
        
        static bool isArray (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true for lists
////////////////////////////////////////////////////////////////////////////////
        
        static bool isList (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true for strings
////////////////////////////////////////////////////////////////////////////////
        
        static bool isString (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true for numbers
////////////////////////////////////////////////////////////////////////////////
        
        static bool isNumber (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true for booleans
////////////////////////////////////////////////////////////////////////////////
        
        static bool isBoolean (struct TRI_json_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns an array sub-element
////////////////////////////////////////////////////////////////////////////////
        
        static struct TRI_json_s* getArrayElement (struct TRI_json_s const*, 
                                                   const char* name);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a string sub-element, or a default it is does not exist
////////////////////////////////////////////////////////////////////////////////
        
        static std::string getStringValue (struct TRI_json_s const*, 
                                           const char*, 
                                           const std::string&);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a numeric sub-element, or a default it is does not exist
////////////////////////////////////////////////////////////////////////////////

        static double getNumberValue (struct TRI_json_s const*, 
                                      const char*, 
                                      double);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a boolean sub-element, or a default it is does not exist
////////////////////////////////////////////////////////////////////////////////
        
        static double getBooleanValue (struct TRI_json_s const*, 
                                       const char*, 
                                       bool);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////
        
    };
  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
