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

#include "Basics/RebootId.h"
#include "Replication2/ReplicatedLog/LogId.h"
#include "Replication2/ReplicatedLog/LogTerm.h"
#include "Replication2/ReplicatedLog/TermIndexPair.h"
#include "Replication2/ReplicatedLog/LocalStateMachineStatus.h"

namespace arangodb::replication2::agency {

struct LogCurrentLocalState {
  LogTerm term{};
  TermIndexPair spearhead{};
  bool snapshotAvailable{false};
  replicated_log::LocalStateMachineStatus state;
  RebootId rebootId = RebootId(0);

  LogCurrentLocalState() = default;
  LogCurrentLocalState(LogTerm term, TermIndexPair spearhead, bool snapshot,
                       RebootId rebootId) noexcept
      : term(term),
        spearhead(spearhead),
        snapshotAvailable(snapshot),
        rebootId(rebootId) {}
  friend auto operator==(LogCurrentLocalState const& s,
                         LogCurrentLocalState const& s2) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2::agency
