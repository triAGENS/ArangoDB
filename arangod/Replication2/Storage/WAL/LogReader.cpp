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

#include "LogReader.h"

#include "Assertions/ProdAssert.h"
#include "Basics/Exceptions.h"
#include "Basics/ResultT.h"
#include "Basics/ScopeGuard.h"
#include "Basics/voc-errors.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/Record.h"

#include <absl/crc/crc32c.h>
#include <velocypack/Validator.h>

#include <string>

namespace arangodb::replication2::storage::wal {

namespace {
constexpr auto minFileSize = sizeof(FileHeader) +
                             sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer);
}

LogReader::LogReader(std::unique_ptr<IFileReader> reader)
    : _reader(std::move(reader)) {
  TRI_ASSERT(_reader->position() == 0);
  FileHeader header;
  auto res = _reader->read(header);
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
                                   "failed to read file header from log file " +
                                       _reader->path() + " - " +
                                       std::string(res.errorMessage()));
  }
  if (header.magic != wMagicFileType) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_INVALID_FILE,
        "invalid file type in log file " + _reader->path());
  }
  if (header.version != wCurrentVersion) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_INVALID_FILE,
        "invalid file version in log file " + _reader->path());
  }
  _firstEntry = _reader->position();
}

LogReader::LogReader(std::unique_ptr<IFileReader> reader,
                     std::uint64_t firstEntry)
    : _reader(std::move(reader)), _firstEntry(firstEntry) {}

void LogReader::seek(std::uint64_t pos) {
  pos = std::max(pos, _firstEntry);
  _reader->seek(pos);
}

auto LogReader::position() const -> std::uint64_t {
  return _reader->position();
}

auto LogReader::size() const -> std::uint64_t { return _reader->size(); }

auto LogReader::seekLogIndexForward(LogIndex index)
    -> ResultT<Record::CompressedHeader> {
  auto pos = _reader->position();

  Record::CompressedHeader header;
  auto res = _reader->read(header);
  if (res.ok()) {
    if (header.index >= index.value) {
      _reader->seek(pos);  // reset to start of entry
      return ResultT<Record::CompressedHeader>::success(header);
    }
    auto prevIndex = header.index;

    while (true) {
      // skip over payload and footer
      pos += sizeof(Record::CompressedHeader) +
             Record::paddedPayloadSize(header.payloadSize) +
             sizeof(Record::Footer);
      _reader->seek(pos);

      res = _reader->read(header);
      if (res.fail()) {
        break;
      }
      ADB_PROD_ASSERT(header.index == prevIndex + 1)
          << "Found non-sequential index in file " << _reader->path()
          << " - pos: " << pos << "; previous index: " << prevIndex
          << "; current index: " << header.index;
      if (header.index >= index.value) {
        _reader->seek(pos);  // reset to start of entry
        return ResultT<Record::CompressedHeader>::success(header);
      }
      prevIndex = header.index;
    }
  }

  // TODO - should we handle the case where the file contains incomplete log
  // entries?

  if (res.fail() and not res.is(TRI_ERROR_END_OF_FILE)) {
    return res;
  }

  return ResultT<Record::CompressedHeader>::error(
      TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
      "log index " + to_string(index) + " not found in file " +
          _reader->path());
}

auto LogReader::seekLogIndexBackward(LogIndex index)
    -> ResultT<Record::CompressedHeader> {
  using RT = ResultT<Record::CompressedHeader>;

  if (_reader->size() <= minFileSize) {
    return RT::error(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
                     "log file " + _reader->path() + " is empty");
  }

  Record::Footer footer;
  Record::CompressedHeader compressedHeader;
  auto pos = _reader->position();
  ADB_PROD_ASSERT(pos % Record::alignment == 0)
      << "file " << _reader->path() << " - pos: " << pos;
  if (pos < minFileSize) {
    return RT::error(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
                     "found corrupt log while searching backwards for index " +
                         to_string(index) + " - cannot read record from file " +
                         _reader->path() + " at position " +
                         std::to_string(pos) +
                         " because it is too small for a single record");
  }
  _reader->seek(pos - sizeof(Record::Footer));

  while (true) {
    auto res = _reader->read(footer);
    ADB_PROD_ASSERT(res.ok())
        << "failed to read footer - " << res.errorMessage();

    // calculate the max entry size based on our current position
    auto maxEntrySize = pos - sizeof(FileHeader);
    ADB_PROD_ASSERT(footer.size % Record::alignment == 0 &&
                    footer.size <= maxEntrySize)
        << "file " << _reader->path() << " - pos: " << pos
        << "; footer.size: " << footer.size
        << "; maxEntrySize: " << maxEntrySize;
    pos -= footer.size;
    // seek to beginning of entry
    _reader->seek(pos);
    res = _reader->read(compressedHeader);
    ADB_PROD_ASSERT(res.ok())
        << "failed to read record header - " << res.errorMessage();

    auto idx = compressedHeader.index;
    if (idx == index.value) {
      ADB_PROD_ASSERT(Record::paddedPayloadSize(compressedHeader.payloadSize) +
                          sizeof(compressedHeader) + sizeof(footer) ==
                      footer.size)
          << "file " << _reader->path() << " - footer.size: " << footer.size
          << "; payloadSize: " << compressedHeader.payloadSize;
      // reset reader to start of entry
      _reader->seek(pos);
      return RT::success(compressedHeader);
    } else if (idx < index.value) {
      return RT::error(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
                       "found index (" + std::to_string(idx) +
                           ") lower than start index (" + to_string(index) +
                           ") while searching backwards");
    } else if (pos <= minFileSize) {
      return RT::error(
          TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
          "found corrupt log while searching backwards for index " +
              to_string(index) + " - cannot read record from file " +
              _reader->path() + " at position " + std::to_string(pos) +
              " because it is less than the minimum file size");
    }
    _reader->seek(pos - sizeof(Record::Footer));
  }
  ADB_UNREACHABLE;
}

auto LogReader::getFirstRecordHeader() -> ResultT<Record::CompressedHeader> {
  auto pos = _reader->position();
  auto guard = scopeGuard([&]() noexcept { _reader->seek(pos); });
  _reader->seek(_firstEntry);
  Record::CompressedHeader header;
  auto res = _reader->read(header);
  if (res.fail()) {
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
        "failed to read record header - " + std::string(res.errorMessage()));
  }
  return ResultT<Record::CompressedHeader>::success(header);
}

auto LogReader::getLastRecordHeader() -> ResultT<Record::CompressedHeader> {
  auto pos = _reader->position();
  auto guard = scopeGuard([&]() noexcept { _reader->seek(pos); });
  auto fileSize = _reader->size();
  if (fileSize < minFileSize) {
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
        "log file " + _reader->path() + " is too small");
  }
  _reader->seek(fileSize - sizeof(Record::Footer));
  Record::Footer footer;
  auto res = _reader->read(footer);
  if (res.fail()) {
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
        "failed to read record footer: " + std::string(res.errorMessage()));
  }
  if (footer.size % 8 != 0 || footer.size > fileSize - sizeof(FileHeader)) {
    LOG_TOPIC("ee7e7", ERR, Logger::REPLICATED_WAL)
        << "Footer at offset " << fileSize - sizeof(Record::Footer) << " in "
        << _reader->path() << " has invalid size: " << footer.size
        << " (file size: " << fileSize << ")";
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
        "invalid footer size in file " + _reader->path());
  }
  _reader->seek(fileSize - footer.size);
  Record::CompressedHeader header;
  res = _reader->read(header);
  if (res.fail()) {
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
        "failed to read record header" + std::string(res.errorMessage()));
  }

  if (Record::paddedPayloadSize(header.payloadSize) + sizeof(header) +
          sizeof(footer) !=
      footer.size) {
    LOG_TOPIC("730f9", ERR, Logger::REPLICATED_WAL)
        << "Record at " << fileSize - footer.size << " in " << _reader->path()
        << " has size that does not match size in footer (payload size: "
        << header.payloadSize << "; footer size: " << footer.size << ")";
    return ResultT<Record::CompressedHeader>::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT,
        "Mismatching size information in file " + _reader->path());
  }

  return ResultT<Record::CompressedHeader>::success(header);
}

auto LogReader::readNextLogEntry() -> ResultT<PersistedLogEntry> {
  using RT = ResultT<PersistedLogEntry>;

  auto startPos = _reader->position();

  Record::CompressedHeader compressedHeader;
  auto res = _reader->read(compressedHeader);
  if (res.fail()) {
    auto error = res.errorNumber() == TRI_ERROR_END_OF_FILE
                     ? TRI_ERROR_END_OF_FILE
                     : TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR;
    return RT::error(error, "failed to read record header - " +
                                std::string(res.errorMessage()));
  }

  auto header = Record::Header{compressedHeader};

  auto paddedSize = Record::paddedPayloadSize(header.payloadSize);
  velocypack::UInt8Buffer buffer(paddedSize);
  res = _reader->read(buffer.data(), paddedSize);
  if (res.fail()) {
    return RT::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
        "failed to read record payload - " + std::string(res.errorMessage()));
  }
  buffer.resetTo(header.payloadSize);

  auto payloadCrc =
      absl::ComputeCrc32c({reinterpret_cast<char const*>(&compressedHeader),
                           sizeof(compressedHeader)});
  payloadCrc = absl::ExtendCrc32c(
      payloadCrc, {reinterpret_cast<char const*>(buffer.data()), paddedSize});

  auto entry = [&]() -> LogEntry {
    if (header.type == RecordType::wMeta) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      velocypack::Validator validator;
      try {
        validator.validate(buffer.data(), buffer.size(), true);
      } catch (std::exception const& ex) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_FAILED,
            "error processing velocypack data for log index " +
                std::to_string(header.index) + " at position " +
                std::to_string(startPos) + " from input file '" +
                _reader->path() + ": " + ex.what());
      }
#endif
      auto payload =
          LogMetaPayload::fromVelocyPack(velocypack::Slice(buffer.data()));
      return LogEntry(LogTerm(header.term), LogIndex(header.index),
                      std::move(payload));
    } else {
      LogPayload payload(std::move(buffer));
      return LogEntry(LogTerm(header.term), LogIndex(header.index),
                      std::move(payload));
    }
  }();

  Record::Footer footer;
  res = _reader->read(footer);
  if (res.fail()) {
    return RT::error(
        TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
        "failed to read record footer - " + std::string(res.errorMessage()));
  }

  ADB_PROD_ASSERT(static_cast<std::uint32_t>(payloadCrc) == footer.crc32)
      << "crc mismatch for log index " << header.index << " at offset "
      << startPos << " - file: " << _reader->path()
      << "; calculated crc: " << payloadCrc << "; stored crc: " << footer.crc32;

  ADB_PROD_ASSERT(footer.size % Record::alignment == 0 &&
                  footer.size == _reader->position() - startPos)
      << "file " << _reader->path() << " - footer.size: " << footer.size
      << "; pos: " << _reader->position() << " startPos: " << startPos;

  auto pos = IteratorPosition::withFileOffset(entry.logIndex(), startPos);

  return PersistedLogEntry(std::move(entry), pos);
}

void LogReader::skipEntry() {
  auto pos = _reader->position();
  Record::CompressedHeader header;
  auto res = _reader->read(header);
  ADB_PROD_ASSERT(res.ok())
      << "failed to read record header - " << res.errorMessage();
  pos += sizeof(Record::CompressedHeader) +
         Record::paddedPayloadSize(header.payloadSize) + sizeof(Record::Footer);
  _reader->seek(pos);
}

}  // namespace arangodb::replication2::storage::wal
