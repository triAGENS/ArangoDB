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

#include <map>
#include <memory>

#include "Basics/Guarded.h"
#include "Replication2/Storage/ILogPersistor.h"
#include "Replication2/Storage/WAL/Options.h"

namespace arangodb::replication2::storage::wal {

struct IFileManager;
struct IFileWriter;

struct LogPersistor : ILogPersistor {
  struct LogFile {
    std::string filename;
    TermIndexPair first;
    TermIndexPair last;

    bool operator==(LogFile const& other) const = default;
  };

  LogPersistor(LogId logId, std::shared_ptr<IFileManager> fileManager,
               Options options);

  [[nodiscard]] auto getIterator(IteratorPosition position)
      -> std::unique_ptr<PersistedLogIterator> override;

  auto insert(std::unique_ptr<LogIterator>, WriteOptions const&)
      -> futures::Future<ResultT<SequenceNumber>> override;
  auto removeFront(LogIndex stop, WriteOptions const&)
      -> futures::Future<ResultT<SequenceNumber>> override;
  auto removeBack(LogIndex start, WriteOptions const&)
      -> futures::Future<ResultT<SequenceNumber>> override;

  auto getLogId() -> LogId override;

  auto waitForSync(SequenceNumber) -> futures::Future<Result> override;

  // waits for all ongoing requests to be done
  void waitForCompletion() noexcept override;

  auto compact() -> Result override;

  auto drop() -> Result override;

  auto fileSet() const -> std::map<LogIndex, LogFile>;

  auto lastWrittenEntry() const -> std::optional<TermIndexPair> {
    return _lastWrittenEntry;
  }

 private:
  struct ActiveFile {
    std::unique_ptr<IFileWriter> writer;
    std::optional<LogIndex> firstIndex;
  };
  struct Files {
    // we map from lastIndex to LogFile, so we can easily find the file
    // containing a specific index note that we need a sorted map with pointer
    // stability!
    std::map<LogIndex, LogFile> fileSet;
    ActiveFile activeFile;
  };

  void loadFileSet();
  [[nodiscard]] auto addToFileSet(Files& f, std::string const& file) -> Result;
  void validateFileSet();
  void createActiveLogFile();

  void finishActiveLogFile(Files& f);
  void createNewActiveLogFile(Files& f);
  auto removeBackFromFile(IFileWriter& writer, LogIndex start)
      -> ResultT<SequenceNumber>;

  LogId const _logId;
  std::shared_ptr<IFileManager> const _fileManager;
  Guarded<Files> _files;
  std::optional<TermIndexPair> _lastWrittenEntry;
  Options _options;
};

std::ostream& operator<<(std::ostream& s, LogPersistor::LogFile const&);

}  // namespace arangodb::replication2::storage::wal
