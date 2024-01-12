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

#include "FileManager.h"

#include <filesystem>
#include <vector>

#include "Assertions/Assert.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "Replication2/Storage/WAL/FileReaderImpl.h"
#include "Replication2/Storage/WAL/FileWriterImpl.h"

namespace arangodb::replication2::storage::wal {

FileManager::FileManager(std::filesystem::path folderPath)
    : _folderPath(std::move(folderPath)) {}

auto FileManager::listFiles() -> std::vector<std::string> {
  std::vector<std::string> result;
  for (auto const& e : std::filesystem::directory_iterator{_folderPath}) {
    TRI_ASSERT(e.is_regular_file());
    if (e.is_regular_file()) {
      result.emplace_back(e.path().filename().string());
    }
  }
  return result;
}

auto FileManager::createReader(std::string const& filename)
    -> std::unique_ptr<IFileReader> {
  auto path = _folderPath / filename;
  LOG_TOPIC("43baa", TRACE, Logger::REPLICATED_WAL)
      << "Creating file reader for " << path.string();
  return std::make_unique<FileReaderImpl>(path.string());
}

auto FileManager::createWriter(std::string const& filename)
    -> std::unique_ptr<IFileWriter> {
  auto path = _folderPath / filename;
  LOG_TOPIC("453d9", TRACE, Logger::REPLICATED_WAL)
      << "Creating file writer for " << path.string();
  return std::make_unique<FileWriterImpl>(path.string());
}

void FileManager::removeAll() {
  LOG_TOPIC("dae4e", INFO, Logger::REPLICATED_WAL)
      << "Removing all files in " << _folderPath.string();
  try {
    std::filesystem::remove_all(_folderPath);
  } catch (std::exception const& ex) {
    LOG_TOPIC("7d944", ERR, Logger::REPLICATED_WAL)
        << "Failed to remove folder  " << _folderPath.string() << ": "
        << ex.what();
    throw;
  }
}

void FileManager::moveFile(std::string_view oldName, std::string_view newName) {
  std::filesystem::rename(_folderPath / oldName, _folderPath / newName);
}

void FileManager::deleteFile(std::string_view filename) {
  std::filesystem::remove(_folderPath / filename);
}

}  // namespace arangodb::replication2::storage::wal
