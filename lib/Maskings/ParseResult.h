////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_MASKINGS_PARSE_RESULT_H
#define ARANGODB_MASKINGS_PARSE_RESULT_H

#include "Basics/Common.h"

template <typename T>
struct ParseResult {
  enum StatusCode : int {
    VALID,
    PARSE_FAILED,
    DUPLICATE_COLLECTION,
    UNKNOWN_TYPE,
    ILLEGAL_PARAMETER
  };

  ParseResult(StatusCode status) : status(status) {}

  ParseResult(StatusCode status, std::string message)
      : status(status), message(message), result(T()) {}

  ParseResult(T&& result)
      : status(StatusCode::VALID), result(std::move(result)) {}

  StatusCode status;
  std::string message;
  T result;
};

#endif
