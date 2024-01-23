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
#include "Inspection/include/Inspection/Format.h"
#include "Replication2/ReplicatedLog/LogId.h"

namespace arangodb::replication2 {

struct GlobalLogIdentifier {
  GlobalLogIdentifier(std::string database, LogId id);
  GlobalLogIdentifier() = default;
  std::string database{};
  LogId id{};

  template<class Inspector>
  inline friend auto inspect(Inspector& f, GlobalLogIdentifier& gid) {
    return f.object(gid).fields(f.field("database", gid.database),
                                f.field("id", gid.id));
  }
};

auto to_string(GlobalLogIdentifier const&) -> std::string;

auto operator<<(std::ostream&, GlobalLogIdentifier const&) -> std::ostream&;

}  // namespace arangodb::replication2

template<>
struct fmt::formatter<arangodb::replication2::GlobalLogIdentifier>
    : arangodb::inspection::inspection_formatter {};
