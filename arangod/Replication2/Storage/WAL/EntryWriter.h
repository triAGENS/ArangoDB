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

namespace arangodb::replication2 {
class LogEntry;
}

namespace arangodb::replication2::storage::wal {

struct Buffer;

struct EntryWriter {
  explicit EntryWriter(Buffer& buffer);
  void appendEntry(LogEntry const& entry);

 private:
  auto writeNormalEntry(LogEntry const& entry) -> std::uint64_t;
  auto writeMetaEntry(LogEntry const& entry) -> std::uint64_t;
  void writePaddingBytes(std::uint32_t payloadSize);
  void writeFooter(std::size_t startPos);

  Buffer& _buffer;
};

}  // namespace arangodb::replication2::storage::wal
