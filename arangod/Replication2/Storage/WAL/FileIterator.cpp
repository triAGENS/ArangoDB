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

#include "FileIterator.h"

#include "Assertions/ProdAssert.h"
#include "Basics/Exceptions.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/Record.h"

#include "velocypack/Buffer.h"

namespace arangodb::replication2::storage::wal {

FileIterator::FileIterator(
    IteratorPosition position, std::unique_ptr<IFileReader> reader,
    std::function<std::unique_ptr<IFileReader>()> moveToNextFile)
    : _reader(std::move(reader)), _moveToNextFile(std::move(moveToNextFile)) {
  _reader.seek(position.fileOffset());
  if (position.index().value != 0) {
    moveToFirstEntry(position);
  }
}

void FileIterator::moveToFirstEntry(IteratorPosition position) {
  auto res = _reader.seekLogIndexForward(position.index());
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res.result());
  }

  TRI_ASSERT(res.get().index >= position.index().value);
}

auto FileIterator::next() -> std::optional<PersistedLogEntry> {
  auto res = _reader.readNextLogEntry();
  if (res.ok()) {
    return {std::move(res.get())};
  }

  TRI_ASSERT(res.fail());
  if (res.errorNumber() != TRI_ERROR_END_OF_FILE) {
    THROW_ARANGO_EXCEPTION(res.result());
  }
  auto fileReader = _moveToNextFile();
  if (!fileReader) {
    return std::nullopt;
  }

  // TODO - simplify this
  _reader.~LogReader();
  new (&_reader) LogReader(std::move(fileReader));

  res = _reader.readNextLogEntry();
  if (res.errorNumber() == TRI_ERROR_END_OF_FILE) {
    // this should only happen if we reach an empty active file
    TRI_ASSERT(_moveToNextFile() == nullptr);
    return std::nullopt;
  }
  return {std::move(res.get())};
}

}  // namespace arangodb::replication2::storage::wal
