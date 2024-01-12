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

#include "Replication2/Storage/WAL/IFileReader.h"

namespace arangodb::replication2::storage::wal::test {

struct MockFileReader : IFileReader {
  MOCK_METHOD(std::string, path, (), (const, override));
  MOCK_METHOD(Result, read, (void*, std::size_t), (override));
  MOCK_METHOD(void, seek, (std::uint64_t), (override));
  MOCK_METHOD(std::uint64_t, position, (), (const, override));
  MOCK_METHOD(std::uint64_t, size, (), (const, override));
};

}  // namespace arangodb::replication2::storage::wal::test
