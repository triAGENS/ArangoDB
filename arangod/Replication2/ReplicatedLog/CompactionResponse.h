////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <variant>

#include "Basics/ErrorCode.h"

#include "Replication2/ReplicatedLog/CompactionResult.h"

namespace arangodb {
template<typename T>
class ResultT;
}
namespace arangodb::replication2::replicated_log {

struct CompactionResponse {
  struct Error {
    ErrorCode error{0};
    std::string errorMessage;

    template<class Inspector>
    friend auto inspect(Inspector& f, Error& x) {
      return f.object(x).fields(f.field("error", x.error),
                                f.field("errorMessage", x.errorMessage));
    }
  };
  static auto fromResult(ResultT<CompactionResult>) -> CompactionResponse;

  std::variant<CompactionResult, Error> value;

  template<class Inspector>
  friend auto inspect(Inspector& f, CompactionResponse& x) {
    namespace insp = arangodb::inspection;
    return f.variant(x.value).embedded("result").alternatives(
        insp::type<CompactionResult>("ok"),
        insp::type<CompactionResponse::Error>("error"));
  }
};

}  // namespace arangodb::replication2::replicated_log
