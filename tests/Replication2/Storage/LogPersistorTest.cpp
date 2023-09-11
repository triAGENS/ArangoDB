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
#include "Replication2/ReplicatedLog/LogEntry.h"
#include "Replication2/ReplicatedLog/LogMetaPayload.h"
#include "Replication2/ReplicatedLog/PersistedLogEntry.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/EntryWriter.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/IFileReader.h"
#include "Replication2/Storage/WAL/LogPersistor.h"
#include "Replication2/Storage/WAL/Options.h"
#include "Replication2/Storage/WAL/Record.h"

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

auto makeNormalLogEntry(std::uint64_t term, std::uint64_t index,
                        std::string payload) {
  return InMemoryLogEntry{LogEntry{LogTerm{term}, LogIndex{index},
                                   LogPayload::createFromString(payload)}};
}

auto makeMetaLogEntry(std::uint64_t term, std::uint64_t index,
                      LogMetaPayload payload) {
  return InMemoryLogEntry{LogEntry{LogTerm{term}, LogIndex{index}, payload}};
}

auto paddedPayloadSize(std::size_t payloadSize) -> std::size_t {
  // we intentionally use a different implementation to calculate the padded
  // size to implicitly test the other paddedPayloadSize implementation
  using arangodb::replication2::storage::wal::Record;
  return ((payloadSize + Record::alignment - 1) / 8) * 8;
}

template<class Func>
auto createBuffer(Func func) -> std::string {
  Buffer buffer;
  buffer.append(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion});

  func(buffer);

  std::string result;
  result.resize(buffer.size());
  std::memcpy(result.data(), buffer.data(), buffer.size());
  return result;
}

auto createEmptyBuffer() -> std::string {
  return createBuffer([](auto&) {});
}

auto createBufferWithLogEntries(std::uint64_t firstIndex,
                                std::uint64_t lastIndex, LogTerm term)
    -> std::string {
  return createBuffer([&](auto& buffer) {
    EntryWriter writer{buffer};
    for (auto i = firstIndex; i < lastIndex + 1; ++i) {
      writer.appendEntry(
          makeNormalLogEntry(term.value, i, "dummyPayload").entry());
    }
  });
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
  std::shared_ptr<MockFileManager> fileManager{
      std::make_shared<MockFileManager>()};

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
  EXPECT_EQ("log file in-memory file is empty", res.errorMessage());
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

  auto& fileSet = persistor->fileSet();
  ASSERT_EQ(3, fileSet.size());
  auto it = fileSet.begin();
  auto checkFile = [](std::string const& filename, LogTerm term,
                      std::uint64_t first, std::uint64_t last, auto actual) {
    LogPersistor::LogFile f{.filename = filename,
                            .first = TermIndexPair(term, LogIndex{first}),
                            .last = TermIndexPair(term, LogIndex{last})};
    EXPECT_EQ(f, actual);
  };
  checkFile("file1", LogTerm{1}, 1, 3, *it);
  ++it;
  checkFile("file2", LogTerm{1}, 4, 5, *it);
  ++it;
  checkFile("file3", LogTerm{2}, 6, 9, *it);
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

  auto& fileSet = persistor->fileSet();
  ASSERT_EQ(2, fileSet.size());
  auto it = fileSet.begin();
  auto checkFile = [](std::string const& filename, LogTerm term,
                      std::uint64_t first, std::uint64_t last, auto actual) {
    LogPersistor::LogFile f{.filename = filename,
                            .first = TermIndexPair(term, LogIndex{first}),
                            .last = TermIndexPair(term, LogIndex{last})};
    EXPECT_EQ(f, actual);
  };
  checkFile("file1", LogTerm{1}, 1, 3, *it);
  ++it;
  checkFile("file5", LogTerm{2}, 4, 6, *it);
  ++it;
  EXPECT_EQ(fileSet.end(), it);

  ASSERT_TRUE(persistor->lastWrittenEntry().has_value());
  EXPECT_EQ(TermIndexPair(LogTerm{2}, LogIndex{6}),
            persistor->lastWrittenEntry().value());
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

    ASSERT_FALSE(persistor->fileSet().empty());
    auto file = *persistor->fileSet().rbegin();
    EXPECT_EQ(fileToFinish, file.filename);
    EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{index}), file.first);
    EXPECT_EQ(TermIndexPair(LogTerm{1}, LogIndex{index}), file.last);
  };

  insertEntryAndCheckFile(1);
  insertEntryAndCheckFile(2);
  insertEntryAndCheckFile(3);
}

}  // namespace arangodb::replication2::storage::wal::test
