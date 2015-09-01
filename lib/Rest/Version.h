////////////////////////////////////////////////////////////////////////////////
/// @brief server version information
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_VERSION_H
#define ARANGODB_REST_VERSION_H 1

#include "Basics/Common.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

struct TRI_json_t;
struct TRI_memory_zone_s;


namespace triagens {
  namespace rest {

// -----------------------------------------------------------------------------
// --SECTION--                                                     class Version
// -----------------------------------------------------------------------------

    class Version {

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief create the version information
////////////////////////////////////////////////////////////////////////////////

        Version () = delete;
        Version (Version const&) = delete;
        Version& operator= (Version const&) = delete;

// -----------------------------------------------------------------------------
// --SECTION--                                           public static functions
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize
////////////////////////////////////////////////////////////////////////////////

        static void initialise ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get numeric server version
////////////////////////////////////////////////////////////////////////////////

        static int32_t getNumericServerVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get server version
////////////////////////////////////////////////////////////////////////////////

        static std::string getServerVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get V8 version
////////////////////////////////////////////////////////////////////////////////

        static std::string getV8Version ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get OpenSSL version
////////////////////////////////////////////////////////////////////////////////

        static std::string getOpenSSLVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get libev version
////////////////////////////////////////////////////////////////////////////////

        static std::string getLibevVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get zlib version
////////////////////////////////////////////////////////////////////////////////

        static std::string getZLibVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get readline version
////////////////////////////////////////////////////////////////////////////////

        static std::string getReadlineVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get ICU version
////////////////////////////////////////////////////////////////////////////////

        static std::string getICUVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get configure
////////////////////////////////////////////////////////////////////////////////

        static std::string getConfigure ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get configure environment
////////////////////////////////////////////////////////////////////////////////

        static std::string getConfigureEnvironment ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get repository version
////////////////////////////////////////////////////////////////////////////////

        static std::string getRepositoryVersion ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get build date
////////////////////////////////////////////////////////////////////////////////

        static std::string getBuildDate ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return a server version string
////////////////////////////////////////////////////////////////////////////////

        static std::string getVerboseVersionString ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get detailed version information as a (multi-line) string
////////////////////////////////////////////////////////////////////////////////

        static std::string getDetailed ();

////////////////////////////////////////////////////////////////////////////////
/// @brief JSONise all data
////////////////////////////////////////////////////////////////////////////////

        static void getJson (struct TRI_memory_zone_s*, struct TRI_json_t*);

// -----------------------------------------------------------------------------
// --SECTION--                                           public static variables
// -----------------------------------------------------------------------------

      public:

        static std::map<std::string, std::string> Values;

    };

  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
