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

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

#include "Replication2/Storage/WAL/FileWriterImpl.h"
#include "Replication2/Storage/WAL/IFileReader.h"

namespace {
auto generateRandomName(size_t length = 8) -> std::string {
  std::mt19937 rand(std::random_device{}());
  auto randchar = [&]() -> char {
    const char charset[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  std::string str(length, 0);
  std::generate_n(str.begin(), length, randchar);
  return str;
}
}  // namespace

namespace arangodb::replication2::storage::wal::test {

struct FileReaderWriterTest : ::testing::Test {
  void SetUp() override {
    path = std::filesystem::temp_directory_path() / generateRandomName();
    std::filesystem::create_directories(path);
  }

  void TearDown() override { std::filesystem::remove_all(path); }

  std::filesystem::path path;
};

TEST_F(FileReaderWriterTest, append) {
  FileWriterImpl writer{path / "test"};
  std::string data = "Hello World";
  writer.append(data);
  auto reader = writer.getReader();

  std::string buffer;
  buffer.resize(data.size());
  auto res = reader->read(buffer.data(), buffer.size());
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(buffer, data);
}

}  // namespace arangodb::replication2::storage::wal::test
