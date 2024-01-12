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
#include <gmock/gmock.h>
#include <exception>
#include <memory>
#include <stdexcept>

#include "Basics/Result.h"
#include "Basics/ResultT.h"

#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/FileIterator.h"

#include "Replication2/Storage/Helpers.h"
#include "Replication2/Storage/InMemoryLogFile.h"
#include "Replication2/Storage/MockFileReader.h"

namespace arangodb::replication2::storage::wal::test {

using namespace testing;

TEST(FileIteratorTest, create_should_seek_to_IteratorPosition_fileOffset) {
  auto reader = std::make_unique<MockFileReader>();
  auto* mock = reader.get();
  auto moveToNextFile = []() -> std::unique_ptr<IFileReader> {
    throw std::runtime_error("moveToNextFile should not be called");
  };
  EXPECT_CALL(*mock, position()).WillRepeatedly(Return(0));
  EXPECT_CALL(*mock, read(_, sizeof(FileHeader)))
      .WillOnce([](void* buffer, std::size_t size) -> Result {
        EXPECT_EQ(sizeof(FileHeader), size);
        FileHeader hdr = {.magic = wMagicFileType, .version = wCurrentVersion};
        memcpy(buffer, &hdr, sizeof(hdr));
        return {};
      });
  EXPECT_CALL(*mock, seek(42));
  FileIterator it(IteratorPosition::withFileOffset(LogIndex{0}, 42),
                  std::move(reader), moveToNextFile);
}

TEST(FileIteratorTest, create_should_move_to_entry_with_specified_index) {
  std::string buffer = createBufferWithLogEntries(1, 3, LogTerm{1});
  auto reader = std::make_unique<InMemoryFileReader>(buffer);
  auto readerPtr = reader.get();
  auto moveToNextFile = []() -> std::unique_ptr<IFileReader> {
    throw std::runtime_error("moveToNextFile should not be called");
  };
  FileIterator it(IteratorPosition::withFileOffset(LogIndex{2}, 0),
                  std::move(reader), moveToNextFile);

  LogReader logReader(std::make_unique<InMemoryFileReader>(buffer));
  auto pos = readerPtr->position();
  logReader.seek(pos);
  auto res = logReader.readNextLogEntry();
  ASSERT_TRUE(res.ok());
  auto& entry = res.get();
  EXPECT_EQ(pos, entry.position().fileOffset());
  EXPECT_EQ(LogIndex{2}, entry.position().index());
  EXPECT_EQ(LogIndex{2}, entry.entry().logIndex());
}

TEST(FileIteratorTest,
     next_should_return_next_entry_and_move_iterator_forward) {
  std::string buffer = createBufferWithLogEntries(1, 3, LogTerm{1});
  auto reader = std::make_unique<InMemoryFileReader>(buffer);
  auto moveToNextFile = []() -> std::unique_ptr<IFileReader> {
    throw std::runtime_error("moveToNextFile should not be called");
  };
  FileIterator it(IteratorPosition::withFileOffset(LogIndex{2}, 0),
                  std::move(reader), moveToNextFile);
  auto res = it.next();
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(LogIndex{2}, res->position().index());

  res = it.next();
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(LogIndex{3}, res->position().index());
}

TEST(FileIteratorTest,
     next_should_call_moveToNextFile_when_reaching_end_of_file) {
  std::vector<std::string> buffers{
      createBufferWithLogEntries(1, 3, LogTerm{1}),  //
      createBufferWithLogEntries(4, 4, LogTerm{1}),  //
      createEmptyBuffer()};

  auto buffersIt = buffers.begin();
  auto reader = std::make_unique<InMemoryFileReader>(*buffersIt);
  auto moveToNextFile = [&]() -> std::unique_ptr<IFileReader> {
    ++buffersIt;
    if (buffersIt == buffers.end()) {
      return nullptr;
    }
    return std::make_unique<InMemoryFileReader>(*buffersIt);
  };
  FileIterator it(IteratorPosition::withFileOffset(LogIndex{3}, 0),
                  std::move(reader), moveToNextFile);
  auto res = it.next();
  EXPECT_EQ(buffersIt, buffers.begin());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(LogIndex{3}, res->position().index());

  res = it.next();
  EXPECT_EQ(buffersIt, buffers.begin() + 1);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(LogIndex{4}, res->position().index());

  res = it.next();
  EXPECT_EQ(buffersIt, buffers.end());
  EXPECT_FALSE(res.has_value());
}

}  // namespace arangodb::replication2::storage::wal::test
