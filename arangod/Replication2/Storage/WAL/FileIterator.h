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
#include "Replication2/Storage/WAL/LogPersistor.h"
#include "Replication2/Storage/WAL/LogReader.h"

namespace arangodb::replication2::storage::wal {

struct FileIterator : PersistedLogIterator {
  FileIterator(IteratorPosition position, std::unique_ptr<IFileReader> reader,
               std::function<std::unique_ptr<IFileReader>()> moveToNextFile);

  auto next() -> std::optional<PersistedLogEntry> override;

 private:
  void moveToFirstEntry(IteratorPosition position);

  LogReader _reader;
  std::function<std::unique_ptr<IFileReader>()> _moveToNextFile;
};

}  // namespace arangodb::replication2::storage::wal
