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

#include <gmock/gmock.h>

#include "Basics/Exceptions.h"
#include "Basics/ResultT.h"
#include "Basics/voc-errors.h"
#include "Futures/Future.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/InMemoryLogEntry.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogEntry.h"
#include "Replication2/ReplicatedLog/LogMetaPayload.h"
#include "Replication2/ReplicatedLog/PersistedLogEntry.h"
#include "Replication2/Storage/IteratorPosition.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/EntryWriter.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/LogPersistor.h"
#include "Replication2/Storage/WAL/Options.h"
#include "Replication2/Storage/WAL/Record.h"

#include "Replication2/Storage/Helpers.h"
#include "Replication2/Storage/InMemoryLogFile.h"
#include "Replication2/Storage/StreamReader.h"
#include "Replication2/Storage/MockFileManager.h"
#include "velocypack/Builder.h"
#include "velocypack/Slice.h"

#include <absl/crc/crc32c.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <immer/flex_vector_transient.hpp>
#include <initializer_list>
#include <ios>
#include <memory>
#include <string_view>
#include <variant>

using namespace ::testing;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

namespace {

using namespace arangodb::replication2::storage::wal;

auto paddedPayloadSize(std::size_t payloadSize) -> std::size_t {
  // we intentionally use a different implementation to calculate the padded
  // size to implicitly test the other paddedPayloadSize implementation
  using arangodb::replication2::storage::wal::Record;
  return ((payloadSize + Record::alignment - 1) / 8) * 8;
}

void checkFileHeader(StreamReader& reader) {
  auto header = reader.read<FileHeader>();
  EXPECT_EQ(wMagicFileType, header.magic);
  EXPECT_EQ(wCurrentVersion, header.version);
}

void checkLogEntry(StreamReader& reader, LogIndex idx, LogTerm term,
                   RecordType type,
                   std::variant<LogPayload, LogMetaPayload> payload) {
  auto* data = reader.data();
  auto dataSize = reader.size();

  VPackBuilder builder;
  VPackSlice payloadSlice;
  if (std::holds_alternative<LogPayload>(payload)) {
    auto const& p = get<LogPayload>(payload);
    payloadSlice = p.slice();
  } else {
    auto const& p = get<LogMetaPayload>(payload);
    p.toVelocyPack(builder);
    payloadSlice = builder.slice();
  }

  auto expectedSize = sizeof(Record::CompressedHeader) +
                      paddedPayloadSize(payloadSlice.byteSize())  // payload
                      + sizeof(Record::Footer);
  ASSERT_EQ(dataSize, expectedSize);

  auto compressedHeader = reader.read<Record::CompressedHeader>();
  auto header = Record::Header{compressedHeader};

  EXPECT_EQ(idx.value, header.index) << "Log index mismatch";
  EXPECT_EQ(term.value, header.term) << "Log term mismatch";
  EXPECT_EQ(type, header.type) << "Entry type mismatch";
  EXPECT_EQ(payloadSlice.byteSize(), header.payloadSize) << "size mismatch";

  EXPECT_TRUE(memcmp(reader.data(), payloadSlice.getDataPtr(),
                     payloadSlice.byteSize()) == 0)
      << "Payload mismatch";

  auto paddedSize = paddedPayloadSize(header.payloadSize);
  reader.skip(paddedSize);

  auto footer = reader.read<Record::Footer>();

  auto expectedCrc = static_cast<std::uint32_t>(
      absl::ComputeCrc32c({reinterpret_cast<char const*>(data),
                           sizeof(Record::CompressedHeader) + paddedSize}));
  EXPECT_EQ(expectedCrc, footer.crc32);
  EXPECT_EQ(expectedSize, footer.size);
}

void skipEntries(std::unique_ptr<LogIterator>& iter, size_t num) {
  for (size_t i = 0; i < num; ++i) {
    auto v = iter->next();
    ASSERT_TRUE(v.has_value());
  }
}

void checkIterators(std::unique_ptr<PersistedLogIterator> actualIter,
                    std::unique_ptr<LogIterator> expectedIter,
                    size_t expectedSize) {
  ASSERT_NE(nullptr, actualIter);
  ASSERT_NE(nullptr, expectedIter);

  auto actual = actualIter->next();
  auto expected = expectedIter->next();
  size_t count = 0;
  while (expected.has_value()) {
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(expected->logIndex(), actual->entry().logIndex());
    EXPECT_EQ(expected->logTerm(), actual->entry().logTerm());
    if (expected->hasPayload()) {
      ASSERT_TRUE(actual->entry().hasPayload());
      auto* expectedPayload = expected->logPayload();
      ASSERT_NE(nullptr, expectedPayload);
      auto* actualPayload = actual->entry().logPayload();
      ASSERT_NE(nullptr, actualPayload);
      ASSERT_EQ(expectedPayload->byteSize(), actualPayload->byteSize());
      ASSERT_TRUE(
          expectedPayload->slice().binaryEquals(actualPayload->slice()));
    } else {
      ASSERT_TRUE(expected->hasMeta());
      ASSERT_TRUE(actual->entry().hasMeta());
      auto* expectedPayload = expected->meta();
      ASSERT_NE(nullptr, expectedPayload);
      auto* actualPayload = actual->entry().meta();
      ASSERT_NE(nullptr, actualPayload);
      EXPECT_EQ(*expectedPayload, *actualPayload);
    }
    ++count;

    actual = actualIter->next();
    expected = expectedIter->next();
  }
  EXPECT_FALSE(actual.has_value());
  EXPECT_EQ(expectedSize, count);
}

}  // namespace

namespace arangodb::replication2::storage::wal::test {

struct LogPersistorSingleFileTest : ::testing::Test {
  void SetUp() override {
    auto file = std::make_unique<InMemoryFileWriter>(buffer);
    EXPECT_CALL(*fileManager, createWriter("_current.log"))
        .WillOnce(Return(std::move(file)));
    EXPECT_CALL(*fileManager, listFiles)
        .WillOnce(Return(std::vector<std::string>{}));

    ON_CALL(*fileManager, createReader("_current.log"))
        .WillByDefault([this](auto) {
          return std::make_unique<InMemoryFileReader>(buffer);
        });

    persistor =
        std::make_unique<LogPersistor>(LogId{42}, fileManager, Options{});
  }

  void insertEntries() {
    log = InMemoryLog{}.append({
        makeNormalLogEntry(1, 1, "blubb"),
        makeNormalLogEntry(1, 2, "dummyPayload"),
        makeMetaLogEntry(
            1, 3,
            LogMetaPayload::withPing(
                "message",
                // the timepoint is serialized as seconds, so we must avoid
                // sub-second precision for simple equality comparison
                LogMetaPayload::Ping::clock::time_point{
                    std::chrono::milliseconds(123000)})),
        makeNormalLogEntry(1, 4, "entry with somewhat larger payload"),
        makeNormalLogEntry(2, 5, "foobar"),
    });

    auto res =
        persistor->insert(log.getLogIterator(), LogPersistor::WriteOptions{})
            .get();
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.get(), 5);
  }

  std::string buffer;
  std::shared_ptr<StrictMock<MockFileManager>> fileManager{
      std::make_shared<StrictMock<MockFileManager>>()};

  InMemoryLog log;
  std::unique_ptr<LogPersistor> persistor;
};

TEST_F(LogPersistorSingleFileTest, drop_calls_file_manager_removeAll) {
  EXPECT_CALL(*fileManager, removeAll).Times(1);
  persistor->drop();
  Mock::VerifyAndClearExpectations(fileManager.get());
}

TEST_F(LogPersistorSingleFileTest, insert_normal_payload) {
  auto payload = LogPayload::createFromString("foobar");
  log = InMemoryLog{}.append(
      {InMemoryLogEntry{LogEntry{LogTerm{1}, LogIndex{100}, payload}}});

  auto res =
      persistor->insert(log.getLogIterator(), LogPersistor::WriteOptions{})
          .get();
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.get(), 100);

  StreamReader reader{buffer.data(), buffer.size()};
  checkFileHeader(reader);
  checkLogEntry(reader, LogIndex{100}, LogTerm{1}, RecordType::wNormal,
                payload);
}

TEST_F(LogPersistorSingleFileTest, insert_meta_payload) {
  LogMetaPayload::Ping::clock::time_point tp{};
  auto payload = LogMetaPayload::withPing("message", tp);
  log = InMemoryLog{}.append(
      {InMemoryLogEntry{LogEntry{LogTerm{1}, LogIndex{100}, payload}}});

  auto res =
      persistor->insert(log.getLogIterator(), LogPersistor::WriteOptions{})
          .get();
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.get(), 100);

  StreamReader reader{buffer.data(), buffer.size()};
  checkFileHeader(reader);
  checkLogEntry(reader, LogIndex{100}, LogTerm{1}, RecordType::wMeta, payload);
}

TEST_F(LogPersistorSingleFileTest, getIterator) {
  insertEntries();
  auto iter =
      persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{0}));
  ASSERT_NE(nullptr, iter);

  auto logIter = log.getLogIterator();
  checkIterators(std::move(iter), std::move(logIter), 5);
}

TEST_F(LogPersistorSingleFileTest, getIterator_seeks_to_log_index) {
  insertEntries();
  auto iter =
      persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{3}));

  auto logIter = log.getLogIterator();
  skipEntries(logIter, 2);

  checkIterators(std::move(iter), std::move(logIter), 3);
}

TEST_F(LogPersistorSingleFileTest,
       getIterator_for_position_from_returned_entry_seeks_to_same_entry) {
  insertEntries();
  auto iter =
      persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{3}));

  ASSERT_NE(nullptr, iter);
  auto entry = iter->next();
  {
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(LogIndex{3}, entry->entry().logIndex());
    EXPECT_EQ(LogTerm{1}, entry->entry().logTerm());
  }
  iter = persistor->getIterator(entry->position());
  ASSERT_NE(nullptr, iter);
  entry = iter->next();
  {
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(LogIndex{3}, entry->entry().logIndex());
    EXPECT_EQ(LogTerm{1}, entry->entry().logTerm());
  }
}

TEST_F(LogPersistorSingleFileTest, removeBack) {
  insertEntries();

  auto res = persistor->removeBack(LogIndex{3}, {}).get();
  ASSERT_TRUE(res.ok()) << res.errorMessage();

  auto iter =
      persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{0}));
  ASSERT_NE(nullptr, iter);
  auto entry = iter->next();
  {
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(LogIndex{1}, entry->entry().logIndex());
    EXPECT_EQ(LogTerm{1}, entry->entry().logTerm());
  }

  entry = iter->next();
  {
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(LogIndex{2}, entry->entry().logIndex());
    EXPECT_EQ(LogTerm{1}, entry->entry().logTerm());
  }

  entry = iter->next();
  EXPECT_FALSE(entry.has_value());

  {
    auto log = InMemoryLog{}.append({
        makeNormalLogEntry(2, 3, "override"),
    });
    auto res =
        persistor->insert(log.getLogIterator(), LogPersistor::WriteOptions{})
            .get();
    ASSERT_TRUE(res.ok()) << res.errorMessage();
    EXPECT_EQ(res.get(), 3);
  }
}

TEST_F(LogPersistorSingleFileTest, removeBack_fails_no_matching_entry_found) {
  auto res = persistor->removeBack(LogIndex{2}, {}).get();
  ASSERT_TRUE(res.fail());
  EXPECT_EQ(
      "log 42 is empty or corrupt - index 2 is not in file set (last index: "
      "<na>) and the active file is empty",
      res.errorMessage());
}

TEST_F(LogPersistorSingleFileTest, removeBack_fails_if_log_file_corrupt) {
  // we simulate a corrupt log file by writing some garbage in the memory buffer
  buffer = "xxxxyyyyzzzz";
  FileHeader header = {.magic = wMagicFileType, .version = wCurrentVersion};
  TRI_ASSERT(buffer.size() > sizeof(header));
  std::memcpy(buffer.data(), &header, sizeof(header));

  auto res = persistor->removeBack(LogIndex{2}, {}).get();
  ASSERT_TRUE(res.fail());
}

TEST_F(LogPersistorSingleFileTest, removeBack_fails_if_start_index_too_small) {
  {
    auto log = InMemoryLog{}.append({
        makeNormalLogEntry(1, 4, "blubb"),
        makeNormalLogEntry(1, 5, "dummyPayload"),
        makeNormalLogEntry(1, 6, "foobar"),
    });
    auto res =
        persistor->insert(log.getLogIterator(), LogPersistor::WriteOptions{})
            .get();
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.get(), 6);
  }

  auto res = persistor->removeBack(LogIndex{2}, {}).get();
  ASSERT_TRUE(res.fail());
}

TEST_F(LogPersistorSingleFileTest, removeBack_fails_if_start_index_too_large) {
  insertEntries();

  auto res = persistor->removeBack(LogIndex{8}, {}).get();
  ASSERT_TRUE(res.fail());
  EXPECT_EQ(
      "found index (5) lower than start index (7) while searching backwards",
      res.errorMessage());
}

struct LogPersistorMultiFileTest : ::testing::Test {
  void SetUp() override {
    // in case of uninteresting calls we don't want the default behavior that
    // returns nullptr, because this simply leads to segmentation faults.
    // so instead, we simply throw an exception
    ON_CALL(*fileManager, createReader(_))
        .WillByDefault([](auto& file) -> std::unique_ptr<IFileReader> {
          throw std::runtime_error("Unexpected call to createReader(\"" + file +
                                   "\")");
        });
    ON_CALL(*fileManager, createWriter(_))
        .WillByDefault([](auto& file) -> std::unique_ptr<IFileWriter> {
          throw std::runtime_error("Unexpected call to createWriter(\"" + file +
                                   "\")");
        });
  }

  struct CompletedFile {
    std::string filename;
    std::string buffer;
  };

  void initializeCompletedFiles(
      std::initializer_list<CompletedFile> completedFiles) {
    _completedFiles = completedFiles;
    std::vector<std::string> completedFilenames;
    for (auto& e : _completedFiles) {
      completedFilenames.push_back(e.filename);
      EXPECT_CALL(*fileManager, createReader(e.filename))
          .WillOnce(Return(std::make_unique<InMemoryFileReader>(e.buffer)));
    }

    EXPECT_CALL(*fileManager, listFiles()).WillOnce(Return(completedFilenames));
  }

  void initializePersistor(std::initializer_list<CompletedFile> completedFiles,
                           std::string writeBuffer, Options options = {}) {
    initializeCompletedFiles(std::move(completedFiles));

    writeBuffers.emplace_back(std::move(writeBuffer));
    auto activeFile = std::make_unique<InMemoryFileWriter>(writeBuffers.back());
    EXPECT_CALL(*fileManager, createWriter("_current.log"))
        .WillOnce(Return(std::move(activeFile)));

    persistor = std::make_unique<LogPersistor>(LogId{42}, fileManager, options);

    testing::Mock::VerifyAndClearExpectations(fileManager.get());
  }

  std::vector<CompletedFile> _completedFiles;
  std::vector<std::string> writeBuffers;
  std::shared_ptr<StrictMock<MockFileManager>> fileManager =
      std::make_shared<StrictMock<MockFileManager>>();
  std::unique_ptr<LogPersistor> persistor;
};

TEST_F(LogPersistorMultiFileTest, loads_file_set_upon_construction) {
  initializePersistor(
      {{.filename = "file1",
        .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})},
       {.filename = "file2",
        .buffer = createBufferWithLogEntries(4, 5, LogTerm{1})},
       {.filename = "file3",
        .buffer = createBufferWithLogEntries(6, 9, LogTerm{2})}},
      "");

  auto fileSet = persistor->fileSet();
  ASSERT_EQ(3, fileSet.size());
  auto it = fileSet.begin();
  auto checkFile = [](std::string const& filename, LogTerm term,
                      std::uint64_t first, std::uint64_t last, auto actual) {
    LogPersistor::LogFile f{.filename = filename,
                            .first = TermIndexPair(term, LogIndex{first}),
                            .last = TermIndexPair(term, LogIndex{last})};
    EXPECT_EQ(f, actual);
  };
  checkFile("file1", LogTerm{1}, 1, 3, it->second);
  ++it;
  checkFile("file2", LogTerm{1}, 4, 5, it->second);
  ++it;
  checkFile("file3", LogTerm{2}, 6, 9, it->second);
  ++it;
  EXPECT_EQ(fileSet.end(), it);

  ASSERT_TRUE(persistor->lastWrittenEntry().has_value());
  EXPECT_EQ(TermIndexPair(LogTerm{2}, LogIndex{9}),
            persistor->lastWrittenEntry().value());
}

TEST_F(LogPersistorMultiFileTest, loading_file_set_ignores_invalid_files) {
  initializePersistor(
      {{
           .filename = "file1",
           .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})  //
       },
       {
           .filename = "file2",
           .buffer = ""  // completely empty file
       },
       {
           .filename = "file3",
           .buffer = createEmptyBuffer()  // empty file with only FileHeader
       },
       {
           .filename = "file4",
           .buffer = createEmptyBuffer() +
                     "xxx"  // file with FileHeader + some invalid data
       },
       {
           .filename = "file5",
           .buffer = createBufferWithLogEntries(4, 6, LogTerm{2})  //
       },
       {
           .filename = "file6",
           .buffer =
               createBufferWithLogEntries(7, 8, LogTerm{2}) +
               "xxx"  // file with some log entries + plus some invalid data
       }},
      "");

  auto fileSet = persistor->fileSet();
  ASSERT_EQ(2, fileSet.size());
  auto it = fileSet.begin();
  auto checkFile = [](std::string const& filename, LogTerm term,
                      std::uint64_t first, std::uint64_t last, auto actual) {
    LogPersistor::LogFile f{.filename = filename,
                            .first = TermIndexPair(term, LogIndex{first}),
                            .last = TermIndexPair(term, LogIndex{last})};
    EXPECT_EQ(f, actual);
  };
  checkFile("file1", LogTerm{1}, 1, 3, it->second);
  ++it;
  checkFile("file5", LogTerm{2}, 4, 6, it->second);
  ++it;
  EXPECT_EQ(fileSet.end(), it);

  ASSERT_TRUE(persistor->lastWrittenEntry().has_value());
  EXPECT_EQ(TermIndexPair(LogTerm{2}, LogIndex{6}),
            persistor->lastWrittenEntry().value());
}

TEST_F(LogPersistorMultiFileTest,
       loading_file_set_does_not_add_current_log_file_to_set) {
  auto activeBuffer = createBufferWithLogEntries(9, 10, LogTerm{2});

  _completedFiles = {
      {.filename = "file1",
       .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})},
      {.filename = "file2",
       .buffer = createBufferWithLogEntries(4, 8, LogTerm{2})},
  };
  std::vector<std::string> completedFilenames;
  for (auto& e : _completedFiles) {
    completedFilenames.push_back(e.filename);
    EXPECT_CALL(*fileManager, createReader(e.filename))
        .WillOnce(Return(std::make_unique<InMemoryFileReader>(e.buffer)));
  }
  // the active file should not be opened via a createReader call
  completedFilenames.push_back("_current.log");

  EXPECT_CALL(*fileManager, listFiles()).WillOnce(Return(completedFilenames));

  writeBuffers.emplace_back(std::move(activeBuffer));
  auto activeFile = std::make_unique<InMemoryFileWriter>(writeBuffers.back());
  EXPECT_CALL(*fileManager, createWriter("_current.log"))
      .WillOnce(Return(std::move(activeFile)));

  persistor = std::make_unique<LogPersistor>(LogId{42}, fileManager, Options{});

  auto fileSet = persistor->fileSet();
  ASSERT_EQ(2, fileSet.size());
  EXPECT_EQ("file2", fileSet.rbegin()->second.filename);
}

TEST_F(LogPersistorMultiFileTest, loading_file_set_throws_if_set_has_gaps) {
  initializeCompletedFiles(
      {{.filename = "file1",
        .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})},
       {.filename = "file2",
        .buffer = createBufferWithLogEntries(5, 8, LogTerm{2})}});

  try {
    persistor =
        std::make_unique<LogPersistor>(LogId{42}, fileManager, Options{});
    ADD_FAILURE() << "LogPersistor constructor is expected to throw";
  } catch (arangodb::basics::Exception& e) {
    EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, e.code());
    EXPECT_EQ("Found a gap in the file set of log 42", e.message());
  }
}

TEST_F(LogPersistorMultiFileTest,
       construction_reads_last_record_from_active_file_if_it_is_not_empty) {
  initializePersistor({}, createBufferWithLogEntries(1, 3, LogTerm{1}));

  ASSERT_TRUE(persistor->lastWrittenEntry().has_value());
  EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{3}),
            persistor->lastWrittenEntry().value());
}

TEST_F(LogPersistorMultiFileTest,
       construction_writes_file_header_to_newly_created_active_file) {
  initializePersistor({}, "");

  ASSERT_FALSE(persistor->lastWrittenEntry().has_value());
  StreamReader reader{writeBuffers.back()};
  auto header = reader.read<FileHeader>();
  EXPECT_EQ(wMagicFileType, header.magic);
  EXPECT_EQ(wCurrentVersion, header.version);
}

TEST_F(
    LogPersistorMultiFileTest,
    construction_keeps_lastWrittenEntry_empty_if_active_file_is_empty_and_no_other_files_exist) {
  initializePersistor({}, createEmptyBuffer());
  ASSERT_FALSE(persistor->lastWrittenEntry().has_value());
}

TEST_F(
    LogPersistorMultiFileTest,
    construction_reads_lastWrittenEntry_from_file_set_if_active_file_is_empty) {
  initializePersistor(
      {{.filename = "file1",
        .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})}},
      createEmptyBuffer());

  ASSERT_TRUE(persistor->lastWrittenEntry().has_value());
  EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{3}),
            persistor->lastWrittenEntry().value());
}

TEST_F(LogPersistorMultiFileTest,
       insert_starts_new_file_if_threshold_exceeded) {
  initializePersistor(
      {}, "",
      // we set the threshold very low to force a new file after each insert
      Options{.logFileSizeThreshold = 1});
  ASSERT_EQ(1, writeBuffers.size());

  auto insertEntryAndCheckFile = [&](std::uint64_t index) {
    auto fileToFinish = fmt::format("{:06}.log", index);
    EXPECT_CALL(*fileManager, moveFile("_current.log", fileToFinish)).Times(1);
    // after moving the file we will create a reader for it to fetch the first
    // and last entries
    EXPECT_CALL(*fileManager, createReader(fileToFinish)).WillOnce([&](auto) {
      return std::make_unique<InMemoryFileReader>(writeBuffers.back());
    });
    EXPECT_CALL(*fileManager, createWriter("_current.log")).WillOnce([&](auto) {
      writeBuffers.emplace_back();
      return std::make_unique<InMemoryFileWriter>(writeBuffers.back());
    });

    auto res = persistor
                   ->insert(InMemoryLog{}
                                .append({makeNormalLogEntry(1, index, "blubb")})
                                .getLogIterator(),
                            LogPersistor::WriteOptions{})
                   .get();

    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.get(), index);
    testing::Mock::VerifyAndClearExpectations(fileManager.get());

    ASSERT_EQ(index + 1, writeBuffers.size());
    StreamReader reader{writeBuffers[index - 1]};
    checkFileHeader(reader);
    checkLogEntry(reader, LogIndex{index}, LogTerm{1}, RecordType::wNormal,
                  LogPayload::createFromString("blubb"));

    auto fileSet = persistor->fileSet();
    ASSERT_FALSE(fileSet.empty());
    auto file = fileSet.rbegin()->second;
    EXPECT_EQ(fileToFinish, file.filename);
    EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{index}), file.first);
    EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{index}), file.last);
  };

  insertEntryAndCheckFile(1);
  insertEntryAndCheckFile(2);
  insertEntryAndCheckFile(3);
}

TEST_F(LogPersistorMultiFileTest, getIterator) {
  initializePersistor(
      {{.filename = "file1",
        .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})},
       {.filename = "file2",
        .buffer = createBufferWithLogEntries(4, 6, LogTerm{1})},
       {.filename = "file3",
        .buffer = createBufferWithLogEntries(7, 9, LogTerm{2})}},
      createBufferWithLogEntries(10, 12, LogTerm{2}));

  auto log = InMemoryLog{}.append({makeNormalLogEntry(1, 5, "dummyPayload"),
                                   makeNormalLogEntry(1, 6, "dummyPayload"),
                                   makeNormalLogEntry(2, 7, "dummyPayload"),
                                   makeNormalLogEntry(2, 8, "dummyPayload"),
                                   makeNormalLogEntry(2, 9, "dummyPayload"),
                                   makeNormalLogEntry(2, 10, "dummyPayload"),
                                   makeNormalLogEntry(2, 11, "dummyPayload"),
                                   makeNormalLogEntry(2, 12, "dummyPayload")});

  {
    EXPECT_CALL(*fileManager, createReader("file2"))
        .WillOnce(Return(
            std::make_unique<InMemoryFileReader>(_completedFiles[1].buffer)));
    EXPECT_CALL(*fileManager, createReader("file3"))
        .WillOnce(Return(
            std::make_unique<InMemoryFileReader>(_completedFiles[2].buffer)));
    auto iter =
        persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{5}));
    ASSERT_NE(nullptr, iter);

    auto logIter = log.getLogIterator();
    checkIterators(std::move(iter), std::move(logIter), 8);
  }

  {
    EXPECT_CALL(*fileManager, createReader("file2"))
        .WillOnce(Return(
            std::make_unique<InMemoryFileReader>(_completedFiles[1].buffer)));
    EXPECT_CALL(*fileManager, createReader("file3"))
        .WillOnce(Return(
            std::make_unique<InMemoryFileReader>(_completedFiles[2].buffer)));
    auto iter =
        persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{6}));
    ASSERT_NE(nullptr, iter);

    auto logIter = log.getLogIterator();
    logIter->next();
    checkIterators(std::move(iter), std::move(logIter), 7);
  }

  {
    EXPECT_CALL(*fileManager, createReader("file3"))
        .WillOnce(Return(
            std::make_unique<InMemoryFileReader>(_completedFiles[2].buffer)));
    auto iter =
        persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{7}));
    ASSERT_NE(nullptr, iter);

    auto logIter = log.getLogIterator();
    logIter->next();
    logIter->next();
    checkIterators(std::move(iter), std::move(logIter), 6);
  }

  {
    // read only from currently active file
    auto iter =
        persistor->getIterator(IteratorPosition::fromLogIndex(LogIndex{11}));
    ASSERT_NE(nullptr, iter);

    auto log =
        InMemoryLog{}.append({makeNormalLogEntry(2, 11, "dummyPayload"),
                              makeNormalLogEntry(2, 12, "dummyPayload")});
    auto logIter = log.getLogIterator();
    checkIterators(std::move(iter), std::move(logIter), 2);
  }
}

struct LogPersistorMultiFileTest_removeFront : LogPersistorMultiFileTest {
  void SetUp() override {
    LogPersistorMultiFileTest::SetUp();
    initializePersistor(
        {{.filename = "file1",
          .buffer = createBufferWithLogEntries(1, 3, LogTerm{1})},
         {.filename = "file2",
          .buffer = createBufferWithLogEntries(4, 6, LogTerm{1})},
         {.filename = "file3",
          .buffer = createBufferWithLogEntries(7, 9, LogTerm{2})}},
        createBufferWithLogEntries(10, 12, LogTerm{2}));
    ASSERT_EQ(3, _completedFiles.size());
  }
};

TEST_F(LogPersistorMultiFileTest_removeFront,
       remove_some_entries_from_first_file) {
  auto res = persistor->removeFront(LogIndex{2}, {}).get();
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  ASSERT_EQ(3, fileSet.size());

  res = persistor->removeFront(LogIndex{3}, {}).get();
  ASSERT_TRUE(res.ok());
  fileSet = persistor->fileSet();
  ASSERT_EQ(3, fileSet.size());
}

TEST_F(LogPersistorMultiFileTest_removeFront,
       remove_all_entries_from_first_file) {
  EXPECT_CALL(*fileManager, deleteFile("file1"));
  auto res = persistor->removeFront(LogIndex{4}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  ASSERT_EQ(2, fileSet.size());
  auto it = fileSet.begin();
  EXPECT_EQ("file2", it->second.filename);
}

TEST_F(LogPersistorMultiFileTest_removeFront,
       remove_all_entries_from_first_two_files) {
  EXPECT_CALL(*fileManager, deleteFile("file1"));
  EXPECT_CALL(*fileManager, deleteFile("file2"));
  auto res = persistor->removeFront(LogIndex{7}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  ASSERT_EQ(1, fileSet.size());
  auto it = fileSet.begin();
  EXPECT_EQ("file3", it->second.filename);
}

TEST_F(LogPersistorMultiFileTest_removeFront, remove_all_completed_files) {
  EXPECT_CALL(*fileManager, deleteFile("file1"));
  EXPECT_CALL(*fileManager, deleteFile("file2"));
  EXPECT_CALL(*fileManager, deleteFile("file3"));
  auto res = persistor->removeFront(LogIndex{10}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  ASSERT_EQ(0, fileSet.size());
}

TEST_F(LogPersistorMultiFileTest_removeFront,
       multiple_subsequent_removeFront_calls) {
  auto res = persistor->removeFront(LogIndex{2}, {}).get();
  ASSERT_TRUE(res.ok());

  EXPECT_CALL(*fileManager, deleteFile("file1"));
  res = persistor->removeFront(LogIndex{4}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());

  EXPECT_CALL(*fileManager, deleteFile("file2"));
  res = persistor->removeFront(LogIndex{7}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());

  EXPECT_CALL(*fileManager, deleteFile("file3"));
  res = persistor->removeFront(LogIndex{10}, {}).get();
  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  ASSERT_EQ(0, fileSet.size());
}

struct LogPersistorMultiFileTest_removeBack : LogPersistorMultiFileTest {
  void SetUp() override {
    LogPersistorMultiFileTest::SetUp();
    initializePersistor(
        {{.filename = "file1",
          .buffer = createBufferWithLogEntries(2, 3, LogTerm{1})},
         {.filename = "file2",
          .buffer = createBufferWithLogEntries(4, 6, LogTerm{1})},
         {.filename = "file3",
          .buffer = createBufferWithLogEntries(7, 9, LogTerm{2})}},
        createBufferWithLogEntries(10, 12, LogTerm{2}));
    ASSERT_EQ(3, _completedFiles.size());
  }

  void checkLastEntry(LogTerm term, LogIndex index) {
    EXPECT_EQ(TermIndexPair(term, index), persistor->lastWrittenEntry());
    auto it = persistor->getIterator(IteratorPosition::fromLogIndex(index));
    ASSERT_NE(nullptr, it);
    auto entry = it->next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(term, entry->entry().logTerm());
    EXPECT_EQ(index, entry->entry().logIndex());
    EXPECT_FALSE(it->next().has_value());
    testing::Mock::VerifyAndClearExpectations(fileManager.get());
  }
};

TEST_F(LogPersistorMultiFileTest_removeBack,
       remove_some_entries_in_active_file) {
  auto res = persistor->removeBack(LogIndex{11}, {}).get();
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  EXPECT_EQ(3, fileSet.size());
  checkLastEntry(LogTerm{2}, LogIndex{10});
}

TEST_F(LogPersistorMultiFileTest_removeBack,
       remove_all_entries_in_active_file) {
  auto res = persistor->removeBack(LogIndex{10}, {}).get();
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  EXPECT_EQ(3, fileSet.size());
  EXPECT_CALL(*fileManager, createReader("file3")).WillOnce([this](auto) {
    return std::make_unique<InMemoryFileReader>(_completedFiles[2].buffer);
  });
  checkLastEntry(LogTerm{2}, LogIndex{9});
}

TEST_F(LogPersistorMultiFileTest_removeBack,
       remove_some_entries_in_completed_file) {
  EXPECT_CALL(*fileManager, createWriter("file3")).WillOnce([this](auto) {
    return std::make_unique<InMemoryFileWriter>(_completedFiles[2].buffer);
  });
  auto res = persistor->removeBack(LogIndex{9}, {}).get();

  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  EXPECT_EQ(3, fileSet.size());

  TermIndexPair expectedTermIndex(LogTerm{2}, LogIndex{8});
  EXPECT_EQ(expectedTermIndex.index, fileSet.rbegin()->first);
  EXPECT_EQ(expectedTermIndex, fileSet.rbegin()->second.last);

  EXPECT_CALL(*fileManager, createReader("file3")).WillOnce([this](auto) {
    return std::make_unique<InMemoryFileReader>(_completedFiles[2].buffer);
  });
  checkLastEntry(expectedTermIndex.term, expectedTermIndex.index);
}

TEST_F(LogPersistorMultiFileTest_removeBack,
       remove_all_entries_in_completed_file) {
  EXPECT_CALL(*fileManager, deleteFile("file3"));
  auto res = persistor->removeBack(LogIndex{7}, {}).get();

  testing::Mock::VerifyAndClearExpectations(fileManager.get());
  ASSERT_TRUE(res.ok());
  auto fileSet = persistor->fileSet();
  EXPECT_EQ(2, fileSet.size());
  auto it = fileSet.rbegin();
  EXPECT_EQ("file2", it->second.filename);
  EXPECT_CALL(*fileManager, createReader("file2")).WillOnce([this](auto) {
    return std::make_unique<InMemoryFileReader>(_completedFiles[1].buffer);
  });
  checkLastEntry(LogTerm{1}, LogIndex{6});
}

// TODO - do we need a test for the case that _all_ entries are compacted?

}  // namespace arangodb::replication2::storage::wal::test
