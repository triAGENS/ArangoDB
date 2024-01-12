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

#include "Helpers.h"

#include "Replication2/ReplicatedLog/InMemoryLogEntry.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/EntryWriter.h"
#include "Replication2/Storage/WAL/FileHeader.h"

namespace {

using namespace arangodb::replication2::storage::wal;

template<class Func>
auto createBuffer(Func func) -> std::string {
  Buffer buffer;
  buffer.append(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion});

  func(buffer);

  std::string result;
  result.resize(buffer.size());
  std::memcpy(result.data(), buffer.data(), buffer.size());
  return result;
}

}  // namespace
namespace arangodb::replication2::storage::wal::test {

auto createEmptyBuffer() -> std::string {
  return createBuffer([](auto&) {});
}

auto createBufferWithLogEntries(std::uint64_t firstIndex,
                                std::uint64_t lastIndex, LogTerm term)
    -> std::string {
  return createBuffer([&](auto& buffer) {
    EntryWriter writer{buffer};
    for (auto i = firstIndex; i < lastIndex + 1; ++i) {
      writer.appendEntry(
          makeNormalLogEntry(term.value, i, "dummyPayload").entry());
    }
  });
}

auto makeNormalLogEntry(std::uint64_t term, std::uint64_t index,
                        std::string payload) -> InMemoryLogEntry {
  return InMemoryLogEntry{LogEntry{LogTerm{term}, LogIndex{index},
                                   LogPayload::createFromString(payload)}};
}

auto makeMetaLogEntry(std::uint64_t term, std::uint64_t index,
                      LogMetaPayload payload) -> InMemoryLogEntry {
  return InMemoryLogEntry{LogEntry{LogTerm{term}, LogIndex{index}, payload}};
}

}  // namespace arangodb::replication2::storage::wal::test
