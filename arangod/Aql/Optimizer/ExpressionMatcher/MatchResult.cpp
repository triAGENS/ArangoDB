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
#include <unordered_map>
#include <variant>
#include <optional>
#include "Assertions/ProdAssert.h"

#include <Aql/AstNode.h>

#include <fmt/core.h>
#include <fmt/format.h>

namespace arangodb::aql::expression_matcher {

auto toStream(std::ostream& os, MatchResult const& result) -> std::ostream& {
  if (result.isError()) {
    os << fmt::format("[[ERROR]] backtrace\n{}",
                      fmt::join(result.errors(), "\n"));
  }

  os << std::endl;

  auto keys = std::vector<std::string>(result.matches().size());

  std::transform(std::begin(result.matches()), std::end(result.matches()),
                 std::begin(keys), [](auto pair) { return pair.first; });
  os << fmt::format("matches:\n  {}", fmt::join(keys, "\n  "));

  return os;
}

auto toString(MatchResult const& result) -> std::string {
  auto resultStream = std::stringstream{};

  toStream(resultStream, result);

  return resultStream.str();
}

}  // namespace arangodb::aql::expression_matcher
