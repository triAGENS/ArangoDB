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

#include <memory>

#include "Replication2/ReplicatedLog/PersistedLogEntry.h"
#include "Replication2/Storage/WAL/Record.h"

namespace arangodb::replication2::storage::wal {

struct IFileReader;

struct LogReader {
  // constructs a LogReader with the given FileReader
  // Expects that the FileReader is positioned at the start of the file and
  // validates the file header
  explicit LogReader(std::unique_ptr<IFileReader> reader);

  // constructs a LogReader with the given FileReader without validating the
  // file header. Instead the given firstEntry is stored as file offset for the
  // first entry in the file
  // Currently this constructor is only used for testing
  LogReader(std::unique_ptr<IFileReader> reader, std::uint64_t firstEntry);

  void seek(std::uint64_t pos);

  [[nodiscard]] auto position() const -> std::uint64_t;

  [[nodiscard]] auto size() const -> std::uint64_t;

  // Seek to the entry with the specified index in the file, starting from the
  // current position of the reader, either forward or backward
  // In case of success, the reader is positioned at the start of the matching
  // entry.
  auto seekLogIndexForward(LogIndex index) -> ResultT<Record::CompressedHeader>;
  auto seekLogIndexBackward(LogIndex index)
      -> ResultT<Record::CompressedHeader>;

  auto getFirstRecordHeader() -> ResultT<Record::CompressedHeader>;
  auto getLastRecordHeader() -> ResultT<Record::CompressedHeader>;

  // read the next entry, starting from the current position of the reader
  auto readNextLogEntry() -> ResultT<PersistedLogEntry>;
  void skipEntry();

 private:
  std::unique_ptr<IFileReader> _reader;
  std::uint64_t _firstEntry;
};

}  // namespace arangodb::replication2::storage::wal
