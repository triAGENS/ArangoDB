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
#include <string>
#include <unordered_map>

#include "Replication2/ReplicatedLog/CommitFailReason.h"
#include "Replication2/ReplicatedLog/LogTerm.h"
#include "Replication2/ReplicatedLog/ParticipantId.h"
#include "Replication2/ReplicatedLog/Agency/ParticipantsConfig.h"
#include "Replication2/ReplicatedLog/Agency/LogCurrentLocalState.h"
#include "Replication2/ReplicatedLog/Agency/LogCurrentSupervision.h"
#include "Replication2/ReplicatedLog/Agency/LogPlanSpecification.h"
#include "Replication2/ReplicatedLog/Agency/Properties.h"

namespace arangodb::replication2::agency {

struct LogCurrent {
  std::unordered_map<ParticipantId, LogCurrentLocalState> localState;
  std::optional<LogCurrentSupervision> supervision;

  struct Leader {
    ParticipantId serverId;
    LogTerm term;
    // optional because the leader might not have committed anything
    std::optional<ParticipantsConfig> committedParticipantsConfig;
    bool leadershipEstablished;
    // will be set after 5s if leader is unable to establish leadership
    std::optional<replicated_log::CommitFailReason> commitStatus;

    friend auto operator==(Leader const& s, Leader const& s2) noexcept
        -> bool = default;
  };

  // Will be nullopt until a leader has assumed leadership
  std::optional<Leader> leader;

  // Lower bounds of RebootIds used in the last committed log entry.
  std::unordered_map<ParticipantId, RebootId> safeRebootIds;

  // Temporary hack until Actions are de-serializable.
  struct ActionDummy {
    std::string timestamp;
    friend auto operator==(ActionDummy const& s, ActionDummy const& s2) noexcept
        -> bool = default;
  };
  std::vector<ActionDummy> actions;

  LogCurrent() = default;
  friend auto operator==(LogCurrent const& s, LogCurrent const& s2) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2::agency
