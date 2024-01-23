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

#include <cstdint>
#include <iosfwd>
#include <optional>

#include "Replication2/ReplicatedLog/CompactionStopReason.h"
#include "Replication2/ReplicatedLog/LogRange.h"

namespace arangodb::replication2::replicated_log {

struct CompactionResult {
  std::size_t numEntriesCompacted{0};
  LogRange range;
  std::optional<CompactionStopReason> stopReason;

  friend auto operator==(CompactionResult const& left,
                         CompactionResult const& right) noexcept
      -> bool = default;

  template<class Inspector>
  friend auto inspect(Inspector& f, CompactionResult& x) {
    return f.object(x).fields(
        f.field("numEntriesCompacted", x.numEntriesCompacted),
        f.field("stopReason", x.stopReason));
  }
};

}  // namespace arangodb::replication2::replicated_log
