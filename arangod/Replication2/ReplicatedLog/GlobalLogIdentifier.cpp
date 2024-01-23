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

#include "GlobalLogIdentifier.h"

#include "Inspection/include/Inspection/VPack.h"

namespace arangodb::replication2 {

GlobalLogIdentifier::GlobalLogIdentifier(std::string database, LogId id)
    : database(std::move(database)), id(id) {}

auto to_string(GlobalLogIdentifier const& gid) -> std::string {
  VPackBuilder b;
  velocypack::serialize(b, gid);
  return b.toJson();
}

auto operator<<(std::ostream& os, GlobalLogIdentifier const& gid)
    -> std::ostream& {
  return os << to_string(gid);
}

}  // namespace arangodb::replication2
