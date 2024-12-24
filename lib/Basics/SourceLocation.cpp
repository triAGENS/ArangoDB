////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "SourceLocation.h"

#include <iostream>
#include <sstream>

namespace arangodb::basics {

auto operator<<(std::ostream& ostream, SourceLocation const& sourceLocation)
    -> std::ostream& {
  return ostream << sourceLocation.file_name() << ":"
                 << sourceLocation.line()
                 // I guess the column usually isn't useful.
                 // << sourceLocation.column() << ":"
                 << "[" << sourceLocation.function_name() << "]";
}

auto to_string(SourceLocation const& sourceLocation) -> std::string {
  auto ss = std::stringstream{};
  ss << sourceLocation;
  return std::move(ss).str();
}

}  // namespace arangodb::basics
