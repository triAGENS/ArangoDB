////////////////////////////////////////////////////////////////////////////////
/// @brief json (extended) result generator
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
/// @author Achim Brandt
/// @author Copyright 2008-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_FYN_RESULT_GENERATOR_JSON_XRESULT_GENERATOR_H
#define TRIAGENS_FYN_RESULT_GENERATOR_JSON_XRESULT_GENERATOR_H 1

#include "ResultGenerator/JsonResultGenerator.h"

namespace triagens {
  namespace rest {

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief json result generator
    ////////////////////////////////////////////////////////////////////////////////

    class JsonXResultGenerator : public JsonResultGenerator {
      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief set ups generating functions
        ////////////////////////////////////////////////////////////////////////////////

        static void initialise ();

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        ResultGeneratorType type () const {
          return RESULT_GENERATOR_JSONX;
        }

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        string contentType () const {
          return "application/json; charset=utf-8";
        }

      public:
        using JsonResultGenerator::generateAtom;

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        void generateAtom (basics::StringBuffer&, int64_t) const;

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        void generateAtom (basics::StringBuffer&, uint32_t) const;

        ////////////////////////////////////////////////////////////////////////////////
        /// {@inheritDoc}
        ////////////////////////////////////////////////////////////////////////////////

        void generateAtom (basics::StringBuffer&, uint64_t) const;
    };
  }
}

#endif
