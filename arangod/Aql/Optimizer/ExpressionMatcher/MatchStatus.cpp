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
////////////////////////////////////////////////////////////////////////////////
#include "MatchResult.h"

#include <string>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <optional>

#include <Aql/AstNode.h>

#include <fmt/core.h>
#include <fmt/format.h>

namespace arangodb::aql::expression_matcher {

auto toStream(std::ostream& os, MatchStatus const& result) -> std::ostream& {
  if (result.isError()) {
    os << fmt::format("[[ERROR]] backtrace\n{}",
                      fmt::join(result.errors(), "\n"));
  }
  return os;
}

auto toString(MatchStatus const& result) -> std::string {
  auto buffer = std::stringstream{};
  toStream(buffer, result);
  return buffer.str();
}

}  // namespace arangodb::aql::expression_matcher
