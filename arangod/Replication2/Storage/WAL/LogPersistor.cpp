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

#include "LogPersistor.h"

#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Assertions/Assert.h"
#include "Assertions/ProdAssert.h"
#include "Basics/Exceptions.h"
#include "Basics/Result.h"
#include "Basics/ResultT.h"
#include "Basics/voc-errors.h"
#include "Futures/Future.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "Replication2/Storage/IteratorPosition.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/EntryWriter.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/FileIterator.h"
#include "Replication2/Storage/WAL/IFileManager.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/IFileWriter.h"
#include "Replication2/Storage/WAL/LogReader.h"
#include "Replication2/Storage/WAL/Record.h"
#include "velocypack/Builder.h"

namespace arangodb::replication2::storage::wal {

namespace {
std::string activeLogFileName = "_current.log";
}

std::ostream& operator<<(std::ostream& s, LogPersistor::LogFile const& f) {
  return s << f.filename << " [" << f.first << " - " << f.last << "]";
}

LogPersistor::LogPersistor(LogId logId,
                           std::shared_ptr<IFileManager> fileManager,
                           Options options)
    : _logId(logId), _fileManager(std::move(fileManager)), _options(options) {
  LOG_TOPIC("a5ceb", TRACE, Logger::REPLICATED_WAL)
      << "Creating LogPersistor for log " << logId;

  loadFileSet();
  validateFileSet();
  createActiveLogFile();
}

auto LogPersistor::fileSet() const -> std::map<LogIndex, LogFile> {
  return _files.getLockedGuard()->fileSet;
}

void LogPersistor::loadFileSet() {
  for (auto& file : _fileManager->listFiles()) {
    if (file == activeLogFileName) {
      continue;
    }
    try {
      auto res = addToFileSet(_files.getLockedGuard().get(), file);
      if (res.fail()) {
        LOG_TOPIC("b0f4c", WARN, Logger::REPLICATED_WAL)
            << "Ignoring file " << file << " in log " << _logId << " - "
            << res.errorMessage();
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("4550a", WARN, Logger::REPLICATED_WAL)
          << "Ignoring file " << file << " in log " << _logId << ": "
          << ex.what();
    }
  }
}

auto LogPersistor::addToFileSet(Files& f, std::string const& file) -> Result {
  LOG_TOPIC("3fc50", TRACE, Logger::REPLICATED_WAL)
      << "Adding file " << file << " to file set of log " << _logId;

  LogReader reader(_fileManager->createReader(file));
  auto res = reader.getFirstRecordHeader();
  if (res.fail()) {
    return Result{res.errorNumber(), "failed to read the first record - " +
                                         std::string(res.errorMessage())};
  }
  auto first = res.get();

  res = reader.getLastRecordHeader();
  if (res.fail()) {
    return Result{res.errorNumber(), "failed to read the last record - " +
                                         std::string(res.errorMessage())};
  }
  auto last = res.get();

  f.fileSet.emplace(
      last.index,
      LogFile{
          .filename = file,
          .first = TermIndexPair(LogTerm{first.term()}, LogIndex{first.index}),
          .last = TermIndexPair(LogTerm{last.term()}, LogIndex{last.index})});
  return {};
}

void LogPersistor::validateFileSet() {
  _files.doUnderLock([this](Files& f) {
    for (auto it = f.fileSet.begin(); it != f.fileSet.end();) {
      TRI_ASSERT(it->first == it->second.last.index);
      auto next = std::next(it);
      if (next != f.fileSet.end()) {
        if (it->second.last.index + 1 != next->second.first.index) {
          LOG_TOPIC("a9e3c", ERR, Logger::REPLICATED_WAL)
              << "Found a gap in the file set of log " << _logId << " - "
              << " file " << it->second.filename << " ends at log index "
              << it->second.last << " and file " << next->second.filename
              << " starts at log index" << next->first;
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
              "Found a gap in the file set of log " + to_string(_logId));
        }
      }
      it = next;
    }
  });
}

void LogPersistor::createActiveLogFile() {
  _files.doUnderLock([&](Files& f) {
    f.activeFile.writer = _fileManager->createWriter(activeLogFileName);
    f.activeFile.firstIndex = std::nullopt;

    auto fileReader = f.activeFile.writer->getReader();
    if (fileReader->size() > sizeof(FileHeader)) {
      LogReader logReader(std::move(fileReader));
      auto res = logReader.getFirstRecordHeader();
      if (res.fail()) {
        LOG_TOPIC("a2184", ERR, Logger::REPLICATED_WAL)
            << "Failed to read first record from " << fileReader->path();
        THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), res.errorMessage());
      }
      f.activeFile.firstIndex = LogIndex{res.get().index};

      res = logReader.getLastRecordHeader();
      if (res.fail()) {
        LOG_TOPIC("940c2", ERR, Logger::REPLICATED_WAL)
            << "Failed to read last record from " << fileReader->path();
        THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), res.errorMessage());
      }
      auto header = res.get();
      _lastWrittenEntry =
          TermIndexPair(LogTerm{header.term()}, LogIndex{header.index});
    } else {
      // file must either be completely empty (newly created) or contain only
      // the FileHeader
      ADB_PROD_ASSERT(fileReader->size() == 0 ||
                      fileReader->size() == sizeof(FileHeader));
      if (fileReader->size() == 0) {
        // file is empty, so we write the header
        FileHeader header = {.magic = wMagicFileType,
                             .version = wCurrentVersion};
        auto res = f.activeFile.writer->append(header);
        if (res.fail()) {
          LOG_TOPIC("f219e", ERR, Logger::REPLICATED_WAL)
              << "Failed to write file header to "
              << f.activeFile.writer->path();
          THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), res.errorMessage());
        }
      }

      if (!f.fileSet.empty()) {
        _lastWrittenEntry = f.fileSet.rbegin()->second.last;
      }
    }
  });
}

auto LogPersistor::getIterator(IteratorPosition position)
    -> std::unique_ptr<PersistedLogIterator> {
  return _files.doUnderLock([&](Files& f)
                                -> std::unique_ptr<PersistedLogIterator> {
    LOG_TOPIC("a6986", TRACE, Logger::REPLICATED_WAL)
        << "Creating iterator for index " << position.index() << " at offset "
        << position.fileOffset() << " in file " << f.activeFile.writer->path();

    auto it = f.fileSet.lower_bound(position.index());

    if (it == f.fileSet.end()) {  // index must be in active file
      LOG_DEVEL << "getIterator: returning file reader for active file";
      return std::make_unique<FileIterator>(
          position, f.activeFile.writer->getReader(), []() { return nullptr; });
    }

    auto moveToNextFile = [it, this]() mutable {
      return _files.doUnderLock([&](Files& f) -> std::unique_ptr<IFileReader> {
        if (it == f.fileSet.end()) {
          LOG_DEVEL << "moveToNextFile reached end of file set";
          return nullptr;
        }
        ++it;
        if (it == f.fileSet.end()) {
          LOG_DEVEL << "moveToNextFile: returning active file reader";
          return f.activeFile.writer->getReader();
        }
        LOG_DEVEL << "moveToNextFile: returning file reader for "
                  << it->second.filename;
        return _fileManager->createReader(it->second.filename);
      });
    };
    LOG_DEVEL << "getIterator: returning file reader for "
              << it->second.filename;
    return std::make_unique<FileIterator>(
        position, _fileManager->createReader(it->second.filename),
        std::move(moveToNextFile));
  });
}

auto LogPersistor::insert(std::unique_ptr<LogIterator> iter,
                          WriteOptions const& writeOptions)
    -> futures::Future<ResultT<SequenceNumber>> {
  Buffer buffer;
  EntryWriter entryWriter(buffer);

  SequenceNumber seq = 0;  // TODO - what if iterator is empty?
  auto lastWrittenEntry = _lastWrittenEntry;
  std::optional<LogIndex> firstEntry;
  unsigned cnt = 0;
  while (auto e = iter->next()) {
    auto& entry = e.value();
    if (lastWrittenEntry.has_value()) {
      ADB_PROD_ASSERT(entry.logIndex() == lastWrittenEntry->index + 1 &&
                      entry.logTerm() >= lastWrittenEntry->term)
          << "attempting to write log entry " << entry.logTermIndexPair()
          << " after " << lastWrittenEntry.value();
    }
    if (!firstEntry) {
      firstEntry = entry.logIndex();
    }

    ++cnt;
    entryWriter.appendEntry(entry);

    seq = entry.logIndex().value;
    lastWrittenEntry = entry.logTermIndexPair();
  }

  return _files.doUnderLock([&](Files& f) -> ResultT<SequenceNumber> {
    auto res = f.activeFile.writer->append(
        {reinterpret_cast<char const*>(buffer.data()), buffer.size()});
    if (res.fail()) {
      LOG_TOPIC("89261", ERR, Logger::REPLICATED_WAL)
          << "Failed to write " << cnt << " entries (" << buffer.size()
          << " bytes) to file " << f.activeFile.writer->path()
          << "; last written entry is "
          << (_lastWrittenEntry.has_value()
                  ? to_string(_lastWrittenEntry.value())
                  : "<na>");

      return res;
    }
    LOG_TOPIC("6fbfd", TRACE, Logger::REPLICATED_WAL)
        << "Wrote " << cnt << " entries (" << buffer.size()
        << " bytes) to file " << f.activeFile.writer->path()
        << "; last written entry is "
        << (lastWrittenEntry.has_value() ? to_string(lastWrittenEntry.value())
                                         : "<na>");

    _lastWrittenEntry = lastWrittenEntry;
    if (firstEntry.has_value() && !f.activeFile.firstIndex.has_value()) {
      f.activeFile.firstIndex = firstEntry;
    }

    if (writeOptions.waitForSync) {
      f.activeFile.writer->sync();
    }

    if (f.activeFile.writer->size() > _options.logFileSizeThreshold) {
      finishActiveLogFile(f);
      createNewActiveLogFile(f);
    }
    return ResultT<SequenceNumber>::success(seq);
  });
}

void LogPersistor::finishActiveLogFile(Files& f) {
  f.activeFile.writer->sync();
  f.activeFile.writer.reset();
  ADB_PROD_ASSERT(f.activeFile.firstIndex.has_value());
  auto newFileName = fmt::format("{:06}.log", f.activeFile.firstIndex->value);
  LOG_TOPIC("093bb", INFO, Logger::REPLICATED_WAL)
      << "Finishing current log file for log " << _logId
      << " and renaming it to " << newFileName;
  _fileManager->moveFile(activeLogFileName, newFileName);
  auto res = addToFileSet(f, newFileName);
  if (res.fail()) {
    LOG_TOPIC("a674f", FATAL, Logger::REPLICATED_WAL)
        << "Failed to add new file " << newFileName << " to file set of log "
        << _logId << ": " << res.errorMessage();
    std::abort();
  }
}

void LogPersistor::createNewActiveLogFile(Files& f) {
  f.activeFile.writer = _fileManager->createWriter(activeLogFileName);
  f.activeFile.firstIndex = std::nullopt;
  ADB_PROD_ASSERT(f.activeFile.writer->size() == 0);
  FileHeader header = {.magic = wMagicFileType, .version = wCurrentVersion};
  auto res = f.activeFile.writer->append(header);
  if (res.fail()) {
    LOG_TOPIC("db0d5", ERR, Logger::REPLICATED_WAL)
        << "Failed to write file header to " << f.activeFile.writer->path();
    THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), res.errorMessage());
  }
}

auto LogPersistor::removeFront(LogIndex stop, WriteOptions const&)
    -> futures::Future<ResultT<SequenceNumber>> {
  LOG_TOPIC("37378", INFO, Logger::REPLICATED_WAL)
      << "Removing log entries for log " << _logId << " up to " << stop;

  return _files.doUnderLock([&](Files& f) -> ResultT<SequenceNumber> {
    if (f.fileSet.empty()) {
      // nothing to do
      return ResultT<SequenceNumber>::success(0);
    }

    ADB_PROD_ASSERT(stop >= f.fileSet.begin()->second.first.index);
    auto endIt = f.fileSet.lower_bound(stop);
    for (auto it = f.fileSet.begin(); it != endIt;) {
      _fileManager->deleteFile(it->second.filename);
      it = f.fileSet.erase(it);
    }

    return ResultT<SequenceNumber>::success(0);
  });
}

auto LogPersistor::removeBack(LogIndex start, WriteOptions const&)
    -> futures::Future<ResultT<SequenceNumber>> {
  LOG_TOPIC("2545c", INFO, Logger::REPLICATED_WAL)
      << "Removing entries from back starting at " << start << " from log "
      << _logId;
  ADB_PROD_ASSERT(start.value > 0);
  // TODO - use WriteOptions or remove

  return _files.doUnderLock([&](Files& f) -> ResultT<SequenceNumber> {
    auto startIt = f.fileSet.lower_bound(start);
    if (startIt == f.fileSet.end()) {
      // index is not in file set, so it can only be in the active file

      // active file must not be empty!
      if (!f.activeFile.firstIndex.has_value()) {
        auto lastFileSetIndex =
            f.fileSet.empty()
                ? std::string{"<na>"}
                : to_string(f.fileSet.rbegin()->second.last.index);
        return ResultT<SequenceNumber>::error(
            TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
            "log " + to_string(_logId) + " is empty or corrupt - index " +
                std::to_string(start.value) +
                " is not in file set (last index: " + lastFileSetIndex +
                ") and the active file is empty");
      }

      if (f.activeFile.firstIndex.value() == start) {
        TRI_ASSERT(!f.fileSet.empty());
        f.activeFile.writer->truncate(sizeof(FileHeader));
        _lastWrittenEntry = f.fileSet.rbegin()->second.last;
        return ResultT<SequenceNumber>::success(start.value);
      } else {
        return removeBackFromFile(*f.activeFile.writer, start);
      }
    } else {
      // since we found the entry in the file set, we can completely truncate
      // the active file
      f.activeFile.writer->truncate(sizeof(FileHeader));

      // delete all items following startIt
      for (auto it = std::next(startIt); it != f.fileSet.end();) {
        _fileManager->deleteFile(it->second.filename);
        it = f.fileSet.erase(it);
      }
      TRI_ASSERT(std::next(startIt) == f.fileSet.end());

      if (start == startIt->second.first.index) {
        // start index is the first index in the file, so we can just delete the
        // whole file and remove it from the file set
        _fileManager->deleteFile(startIt->second.filename);

        TRI_ASSERT(startIt != f.fileSet.begin());
        auto prev = std::prev(startIt);
        _lastWrittenEntry = prev->second.last;

        f.fileSet.erase(startIt);
        return ResultT<SequenceNumber>::success(start.value);
      } else {
        // we cannot remove the file, but have to truncate it
        auto writer = _fileManager->createWriter(startIt->second.filename);
        auto res = removeBackFromFile(*writer, start);
        if (res.ok()) {
          // we have just removed some entries from the back of the file, so we
          // must updated the tracked index values in the file set. The file set
          // is sorted by the `last` values, but since keys cannot be changed we
          // have to remove and reinsert the entry. _lastWrittenEntry has
          // already been updated accordingly, so we can just use that value
          auto entry = startIt->second;
          entry.last = _lastWrittenEntry.value();
          f.fileSet.erase(startIt);
          f.fileSet.emplace(entry.last.index, entry);
        }
        return res;
      }
    }
  });
}

auto LogPersistor::removeBackFromFile(IFileWriter& writer, LogIndex start)
    -> ResultT<SequenceNumber> try {
  LogReader reader(writer.getReader());
  reader.seek(reader.size());

  // we seek the predecessor of start, because we want to get its term
  auto lookupIndex = start.saturatedDecrement();
  auto res = reader.seekLogIndexBackward(lookupIndex);
  if (res.fail()) {
    LOG_TOPIC("93e92", ERR, Logger::REPLICATED_WAL)
        << "Failed to located entry with index " << lookupIndex << " in log "
        << _logId << ": " << res.errorMessage();
    return res.result();
  }

  auto header = res.get();
  TRI_ASSERT(header.index + 1 == start.value);
  // we located the predecessor, now we skip over it so we find the offset
  // at which we need to truncate
  reader.skipEntry();

  _lastWrittenEntry =
      TermIndexPair(LogTerm{header.term()}, LogIndex{header.index});

  auto newSize = reader.position();
  LOG_TOPIC("a1db0", INFO, Logger::REPLICATED_WAL)
      << "Truncating file " << writer.path() << " at " << newSize;

  writer.truncate(newSize);
  return ResultT<SequenceNumber>::success(start.value);
} catch (basics::Exception& ex) {
  LOG_TOPIC("7741d", ERR, Logger::REPLICATED_WAL)
      << "Failed to remove entries from back of file " << writer.path() << ": "
      << ex.message();
  return ResultT<SequenceNumber>::error(ex.code(), ex.message());
} catch (std::exception& ex) {
  LOG_TOPIC("08da6", FATAL, Logger::REPLICATED_WAL)
      << "Failed to remove entries from back of file " << writer.path() << ": "
      << ex.what();
  return ResultT<SequenceNumber>::error(
      TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, ex.what());
}

auto LogPersistor::getLogId() -> LogId { return _logId; }

auto LogPersistor::waitForSync(SequenceNumber) -> futures::Future<Result> {
  // TODO
  return Result{};
}

// waits for all ongoing requests to be done
void LogPersistor::waitForCompletion() noexcept {
  // TODO
}

auto LogPersistor::compact() -> Result {
  // nothing to do here - this function should be removed from the interface
  return Result{};
}

auto LogPersistor::drop() -> Result {
  LOG_TOPIC("8fb77", INFO, Logger::REPLICATED_WAL)
      << "Dropping LogPersistor for log " << _logId;
  // TODO - error handling
  _files.getLockedGuard()->activeFile.writer.reset();
  _fileManager->removeAll();
  return Result{};
}

}  // namespace arangodb::replication2::storage::wal
