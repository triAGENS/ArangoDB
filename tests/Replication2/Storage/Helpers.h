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

#include <cstdint>
#include <string>

#include "Replication2/ReplicatedLog/InMemoryLogEntry.h"
#include "Replication2/ReplicatedLog/LogCommon.h"

namespace arangodb::replication2::storage::wal::test {

auto createEmptyBuffer() -> std::string;

auto createBufferWithLogEntries(std::uint64_t firstIndex,
                                std::uint64_t lastIndex, LogTerm term)
    -> std::string;

auto makeNormalLogEntry(std::uint64_t term, std::uint64_t index,
                        std::string payload) -> InMemoryLogEntry;

auto makeMetaLogEntry(std::uint64_t term, std::uint64_t index,
                      LogMetaPayload payload) -> InMemoryLogEntry;

}  // namespace arangodb::replication2::storage::wal::test
