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

#pragma once

#include "Basics/StaticStrings.h"
#include "Replication2/ReplicatedLog/LogIndex.h"
#include "Replication2/ReplicatedLog/TermIndexPair.h"

namespace arangodb::velocypack {
class Builder;
class Slice;
}  // namespace arangodb::velocypack

namespace arangodb::replication2::replicated_log {

struct LogStatistics {
  TermIndexPair spearHead{};
  LogIndex commitIndex{};
  LogIndex firstIndex{};
  LogIndex releaseIndex{};
  LogIndex syncIndex{};
  LogIndex lowestIndexToKeep{};

  void toVelocyPack(velocypack::Builder& builder) const;
  [[nodiscard]] static auto fromVelocyPack(velocypack::Slice slice)
      -> LogStatistics;

  friend auto operator==(LogStatistics const& left,
                         LogStatistics const& right) noexcept -> bool = default;
};

template<class Inspector>
auto inspect(Inspector& f, LogStatistics& x) {
  using namespace arangodb;
  return f.object(x).fields(
      f.field(StaticStrings::Spearhead, x.spearHead),
      f.field(StaticStrings::CommitIndex, x.commitIndex),
      f.field(StaticStrings::FirstIndex, x.firstIndex),
      f.field(StaticStrings::ReleaseIndex, x.releaseIndex),
      f.field(StaticStrings::SyncIndex, x.syncIndex));
}

}  // namespace arangodb::replication2::replicated_log
