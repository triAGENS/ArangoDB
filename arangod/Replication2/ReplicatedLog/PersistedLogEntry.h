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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <Assertions/Assert.h>
#include "Replication2/ReplicatedLog/LogEntry.h"
#include "Replication2/Storage/IteratorPosition.h"

namespace arangodb::replication2 {

// A log entry enriched with additional information about the position of where
// this entry is stored. This allows us to efficiently acquire an iterator
// starting at this entry.
struct PersistedLogEntry {
  PersistedLogEntry(LogEntry&& entry, storage::IteratorPosition position)
      : _entry(std::move(entry)), _position(position) {
    TRI_ASSERT(_entry.logIndex() == _position.index());
  }

  [[nodiscard]] auto entry() const noexcept -> LogEntry const& {
    return _entry;
  }

  [[nodiscard]] auto position() const noexcept -> storage::IteratorPosition {
    return _position;
  }

 private:
  LogEntry _entry;
  storage::IteratorPosition _position;
};

struct PersistedLogIterator : TypedLogIterator<PersistedLogEntry> {};

}  // namespace arangodb::replication2
