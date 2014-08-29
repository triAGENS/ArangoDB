////////////////////////////////////////////////////////////////////////////////
/// @brief Aql, query context
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
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_AQL_QUERY_H
#define ARANGODB_AQL_QUERY_H 1

#include "Basics/Common.h"
#include "Basics/JsonHelper.h"
#include "Aql/BindParameters.h"
#include "Aql/Collections.h"
#include "Aql/QueryResult.h"

struct TRI_json_s;
struct TRI_vocbase_s;

namespace triagens {
  namespace aql {

    class V8Executor;
    class Expression;
    struct Variable;
    struct AstNode;

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief the type of query to execute
////////////////////////////////////////////////////////////////////////////////

    enum QueryType {
      AQL_QUERY_READ,
      AQL_QUERY_REMOVE,
      AQL_QUERY_INSERT,
      AQL_QUERY_UPDATE,
      AQL_QUERY_REPLACE
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                       class Query
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief an AQL query
////////////////////////////////////////////////////////////////////////////////

    class Query {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

      public:

        Query (struct TRI_vocbase_s*,
               char const*,
               size_t,
               struct TRI_json_s*,
               struct TRI_json_s*);

        Query (struct TRI_vocbase_s*,
               triagens::basics::Json queryStruct,
               QueryType Type);

        ~Query ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief get the vocbase
////////////////////////////////////////////////////////////////////////////////

        inline struct TRI_vocbase_s* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief collections
////////////////////////////////////////////////////////////////////////////////

        inline Collections* collections () {
          return &_collections;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the names of collections used in the query
////////////////////////////////////////////////////////////////////////////////

        std::vector<std::string> collectionNames () const {
          return _collections.collectionNames();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the query type
////////////////////////////////////////////////////////////////////////////////

        inline QueryType type () const {
          return _type;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the query type
////////////////////////////////////////////////////////////////////////////////

        void type (QueryType type) {
          _type = type;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the query string
////////////////////////////////////////////////////////////////////////////////

        char const* queryString () const {
          return _queryString;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the length of the query string
////////////////////////////////////////////////////////////////////////////////

        size_t queryLength () const {
          return _queryLength;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief extract a region from the query
////////////////////////////////////////////////////////////////////////////////

        std::string extractRegion (int, 
                                   int) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief register an error
////////////////////////////////////////////////////////////////////////////////

        void registerError (int,
                            char const* = nullptr);

////////////////////////////////////////////////////////////////////////////////
/// @brief execute an AQL query 
////////////////////////////////////////////////////////////////////////////////

        QueryResult execute ();

////////////////////////////////////////////////////////////////////////////////
/// @brief parse an AQL query
////////////////////////////////////////////////////////////////////////////////

        QueryResult parse ();

////////////////////////////////////////////////////////////////////////////////
/// @brief explain an AQL query 
////////////////////////////////////////////////////////////////////////////////

        QueryResult explain ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get v8 executor
////////////////////////////////////////////////////////////////////////////////

        V8Executor* executor ();

////////////////////////////////////////////////////////////////////////////////
/// @brief register a string
/// the string is freed when the query is destroyed
////////////////////////////////////////////////////////////////////////////////

        char* registerString (char const*,
                              size_t,
                              bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief register a string
/// the string is freed when the query is destroyed
////////////////////////////////////////////////////////////////////////////////

        char* registerString (std::string const&,
                              bool);

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief read the "optimizer.rules" section from the options
////////////////////////////////////////////////////////////////////////////////

        std::vector<std::string> getRulesFromOptions () const;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief pointer to vocbase the query runs in
////////////////////////////////////////////////////////////////////////////////

        struct TRI_vocbase_s*      _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief V8 code executor
////////////////////////////////////////////////////////////////////////////////
        
        V8Executor*                _executor;

////////////////////////////////////////////////////////////////////////////////
/// @brief the actual query string
////////////////////////////////////////////////////////////////////////////////

        char const*                _queryString;

////////////////////////////////////////////////////////////////////////////////
/// @brief length of the query string in bytes
////////////////////////////////////////////////////////////////////////////////

        size_t const               _queryLength;

////////////////////////////////////////////////////////////////////////////////
/// @brief query in a JSON structure
////////////////////////////////////////////////////////////////////////////////

        triagens::basics::Json const _queryJson;

////////////////////////////////////////////////////////////////////////////////
/// @brief type of the query
////////////////////////////////////////////////////////////////////////////////

        QueryType                  _type;

////////////////////////////////////////////////////////////////////////////////
/// @brief bind parameters for the query
////////////////////////////////////////////////////////////////////////////////

        BindParameters             _bindParameters;

////////////////////////////////////////////////////////////////////////////////
/// @brief query options
////////////////////////////////////////////////////////////////////////////////

        TRI_json_t*                _options;

////////////////////////////////////////////////////////////////////////////////
/// @brief collections used in the query
////////////////////////////////////////////////////////////////////////////////

        Collections                _collections;

////////////////////////////////////////////////////////////////////////////////
/// @brief all strings created in the query - used for easy memory deallocation
////////////////////////////////////////////////////////////////////////////////

        std::vector<char const*>   _strings;
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
