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
#include <optional>

#include "Basics/StaticStrings.h"
#include "Replication2/ReplicatedLog/LogTerm.h"
#include "Replication2/ReplicatedLog/Agency/ServerInstanceReference.h"

namespace arangodb::replication2::agency {

struct LogPlanTermSpecification {
  LogTerm term;
  std::optional<ServerInstanceReference> leader;

  LogPlanTermSpecification() = default;

  LogPlanTermSpecification(LogTerm term,
                           std::optional<ServerInstanceReference>);

  friend auto operator==(LogPlanTermSpecification const&,
                         LogPlanTermSpecification const&) noexcept
      -> bool = default;
  friend auto operator<<(std::ostream& os, LogPlanTermSpecification const&)
      -> std::ostream&;
};

template<class Inspector>
auto inspect(Inspector& f, LogPlanTermSpecification& x) {
  return f.object(x).fields(f.field(StaticStrings::Term, x.term),
                            f.field(StaticStrings::Leader, x.leader));
}

auto operator<<(std::ostream& os, LogPlanTermSpecification const&)
    -> std::ostream&;

}  // namespace arangodb::replication2::agency
