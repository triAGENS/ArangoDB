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

#include <fmt/core.h>

#include "Basics/RebootId.h"
#include "Basics/StaticStrings.h"
#include "Replication2/ReplicatedLog/ParticipantId.h"

namespace arangodb::replication2::agency {

struct ServerInstanceReference {
  ParticipantId serverId;
  RebootId rebootId;

  ServerInstanceReference(ParticipantId participant, RebootId rebootId)
      : serverId{std::move(participant)}, rebootId{rebootId} {}
  ServerInstanceReference() : rebootId{RebootId{0}} {};
  friend auto operator==(ServerInstanceReference const&,
                         ServerInstanceReference const&) noexcept
      -> bool = default;
};

template<class Inspector>
auto inspect(Inspector& f, ServerInstanceReference& x) {
  return f.object(x).fields(f.field(StaticStrings::ServerId, x.serverId),
                            f.field(StaticStrings::RebootId, x.rebootId));
}

auto operator<<(std::ostream&, ServerInstanceReference const&) noexcept
    -> std::ostream&;

}  // namespace arangodb::replication2::agency

template<>
struct fmt::formatter<arangodb::replication2::agency::ServerInstanceReference>
    : formatter<string_view> {
  // parse is inherited from formatter<string_view>.
  template<typename FormatContext>
  auto format(
      arangodb::replication2::agency::ServerInstanceReference const& inst,
      FormatContext& ctx) const {
    return fmt::format_to(ctx.out(), "{}@{}", inst.serverId,
                          inst.rebootId.value());
  }
};
