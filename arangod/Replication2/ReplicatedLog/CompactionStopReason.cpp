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

#include "CompactionStopReason.h"

#include <ostream>
#include <Basics/StaticStrings.h>
#include <Basics/StringUtils.h>
#include <Basics/VelocyPackHelper.h>
#include <Basics/debugging.h>
#include <Inspection/VPack.h>

#include <fmt/core.h>

namespace arangodb::replication2::replicated_log {

auto operator<<(std::ostream& os, CompactionStopReason const& csr)
    -> std::ostream& {
  return os << to_string(csr);
}

auto to_string(CompactionStopReason const& csr) -> std::string {
  struct ToStringVisitor {
    auto operator()(CompactionStopReason::LeaderBlocksReleaseEntry const&)
        -> std::string {
      return "Leader prevents release of more log entries";
    }
    auto operator()(CompactionStopReason::NothingToCompact const&)
        -> std::string {
      return "Nothing to compact";
    }
    auto operator()(
        CompactionStopReason::NotReleasedByStateMachine const& reason)
        -> std::string {
      return fmt::format("Statemachine release index is at {}",
                         reason.releasedIndex.value);
    }
    auto operator()(
        CompactionStopReason::CompactionThresholdNotReached const& reason)
        -> std::string {
      return fmt::format(
          "Automatic compaction threshold not reached, next compaction at {}",
          reason.nextCompactionAt.value);
    }
    auto operator()(
        CompactionStopReason::ParticipantMissingEntries const& reason)
        -> std::string {
      return fmt::format(
          "Compaction waiting for participant {} to receive all log entries",
          reason.who);
    }
  };

  return std::visit(ToStringVisitor{}, csr.value);
}

}  // namespace arangodb::replication2::replicated_log
