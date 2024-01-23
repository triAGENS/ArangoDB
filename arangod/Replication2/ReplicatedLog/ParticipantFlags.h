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

#include <iosfwd>

namespace arangodb::replication2 {

struct ParticipantFlags {
  bool forced = false;
  bool allowedInQuorum = true;
  bool allowedAsLeader = true;

  friend auto operator==(ParticipantFlags const& left,
                         ParticipantFlags const& right) noexcept
      -> bool = default;

  friend auto operator<<(std::ostream&, ParticipantFlags const&)
      -> std::ostream&;
};

template<class Inspector>
auto inspect(Inspector& f, ParticipantFlags& x) {
  return f.object(x).fields(
      f.field("forced", x.forced).fallback(false),
      f.field("allowedInQuorum", x.allowedInQuorum).fallback(true),
      f.field("allowedAsLeader", x.allowedAsLeader).fallback(true));
}

auto operator<<(std::ostream&, ParticipantFlags const&) -> std::ostream&;

}  // namespace arangodb::replication2
