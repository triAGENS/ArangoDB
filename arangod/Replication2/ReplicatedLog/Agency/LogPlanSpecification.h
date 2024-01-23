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

#include "Replication2/ReplicatedLog/LogId.h"
#include "Replication2/ReplicatedLog/Agency/LogPlanConfig.h"
#include "Replication2/ReplicatedLog/Agency/LogPlanTermSpecification.h"
#include "Replication2/ReplicatedLog/Agency/ParticipantsConfig.h"
#include "Replication2/ReplicatedLog/Agency/Properties.h"

namespace arangodb::replication2::agency {

struct LogPlanSpecification {
  LogId id;
  std::optional<LogPlanTermSpecification> currentTerm;

  ParticipantsConfig participantsConfig;
  Properties properties;

  std::optional<std::string> owner;

  LogPlanSpecification() = default;

  LogPlanSpecification(LogId id, std::optional<LogPlanTermSpecification> term);
  LogPlanSpecification(LogId id, std::optional<LogPlanTermSpecification> term,
                       ParticipantsConfig participantsConfig);

  friend auto operator==(LogPlanSpecification const&,
                         LogPlanSpecification const&) noexcept
      -> bool = default;
  friend auto operator<<(std::ostream& os, LogPlanSpecification const&)
      -> std::ostream&;
};

auto operator<<(std::ostream& os, LogPlanSpecification const&) -> std::ostream&;

}  // namespace arangodb::replication2::agency
