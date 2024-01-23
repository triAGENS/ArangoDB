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
#include "Replication2/ReplicatedLog/Agency/LogTargetConfig.h"
#include "Replication2/ReplicatedLog/Agency/ParticipantsConfig.h"
#include "Replication2/ReplicatedLog/Agency/Properties.h"

namespace arangodb::replication2::agency {

struct LogTarget {
  LogId id;
  ParticipantsFlagsMap participants;
  LogTargetConfig config;
  Properties properties;

  std::optional<ParticipantId> leader;
  std::optional<uint64_t> version;

  struct Supervision {
    std::size_t maxActionsTraceLength{0};
    friend auto operator==(Supervision const&, Supervision const&) noexcept
        -> bool = default;
  };

  std::optional<Supervision> supervision;
  std::optional<std::string> owner;

  LogTarget() = default;

  LogTarget(LogId id, ParticipantsFlagsMap participants,
            LogTargetConfig const& config)
      : id{id}, participants{std::move(participants)}, config(config) {}

  friend auto operator==(LogTarget const&, LogTarget const&) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2::agency
