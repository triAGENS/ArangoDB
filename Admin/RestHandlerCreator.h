////////////////////////////////////////////////////////////////////////////////
/// @brief creator function
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

#ifndef TRIAGENS_BORNHOLM_ADMIN_REST_HANDLER_CREATOR_H
#define TRIAGENS_BORNHOLM_ADMIN_REST_HANDLER_CREATOR_H 1

#include <Basics/Common.h>

namespace triagens {
  namespace rest {
    class HttpHandler;
    class HttpRequest;
  }

  namespace admin {

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief creator function
    ////////////////////////////////////////////////////////////////////////////////

    template<typename H>
    class RestHandlerCreator : public H {
      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief create with void pointer data
        ////////////////////////////////////////////////////////////////////////////////

        static rest::HttpHandler* create (rest::HttpRequest* request, void* data) {
          return new H(request, data);
        }

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief create with data
        ////////////////////////////////////////////////////////////////////////////////

        template<typename D>
        static rest::HttpHandler* createData (rest::HttpRequest* request, void* data) {
          return new H(request, (D) data);
        }

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief create without data
        ////////////////////////////////////////////////////////////////////////////////

        static rest::HttpHandler* createNoData (rest::HttpRequest* request, void*) {
          return new H(request);
        }
    };
  }
}

#endif
