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

#include <optional>

#include "Replication2/ReplicatedLog/Agency/LogCurrent.h"
#include "Replication2/ReplicatedLog/Agency/LogPlanSpecification.h"
#include "Replication2/ReplicatedLog/Agency/LogTarget.h"

namespace arangodb::replication2::agency {

/* Convenience Wrapper */
struct Log {
  LogTarget target;

  // These two do not necessarily exist in the Agency
  // so when we're called for a Log these might not
  // exist
  std::optional<LogPlanSpecification> plan;
  std::optional<LogCurrent> current;
  friend auto operator==(Log const& s, Log const& s2) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2::agency
