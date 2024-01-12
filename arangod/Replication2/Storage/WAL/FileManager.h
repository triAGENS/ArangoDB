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

#include "Replication2/Storage/WAL/IFileManager.h"

#include <filesystem>

namespace arangodb::replication2::storage::wal {

struct FileManager : IFileManager {
  explicit FileManager(std::filesystem::path folderPath);

  auto listFiles() -> std::vector<std::string> override;

  auto createReader(std::string const& filename)
      -> std::unique_ptr<IFileReader> override;
  auto createWriter(std::string const& filename)
      -> std::unique_ptr<IFileWriter> override;

  void removeAll() override;

  void moveFile(std::string_view oldName, std::string_view newName) override;

  void deleteFile(std::string_view filename) override;

 private:
  std::filesystem::path _folderPath;
};

}  // namespace arangodb::replication2::storage::wal
