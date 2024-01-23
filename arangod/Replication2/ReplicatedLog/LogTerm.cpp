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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "LogTerm.h"

#include <ostream>
namespace arangodb::replication2 {

LogTerm::operator velocypack::Value() const noexcept {
  return velocypack::Value(value);
}

auto operator<<(std::ostream& os, LogTerm term) -> std::ostream& {
  return os << term.value;
}

auto LogTerm::succ() const noexcept -> LogTerm { return LogTerm(value + 1); }

auto to_string(LogTerm term) -> std::string {
  return std::to_string(term.value);
}

}  // namespace arangodb::replication2
