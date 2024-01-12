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

#include <gmock/gmock.h>
#include <string_view>

#include "Basics/Result.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/IFileWriter.h"
#include "Replication2/Storage/WAL/IFileManager.h"

namespace arangodb::replication2::storage::wal::test {

struct MockFileManager : IFileManager {
  MOCK_METHOD(std::vector<std::string>, listFiles, (), (override));
  MOCK_METHOD(std::unique_ptr<IFileReader>, createReader, (std::string const&),
              (override));
  MOCK_METHOD(std::unique_ptr<IFileWriter>, createWriter, (std::string const&),
              (override));
  MOCK_METHOD(void, removeAll, (), (override));
  MOCK_METHOD(void, moveFile, (std::string_view, std::string_view), (override));
  MOCK_METHOD(void, deleteFile, (std::string_view), (override));
};

}  // namespace arangodb::replication2::storage::wal::test
