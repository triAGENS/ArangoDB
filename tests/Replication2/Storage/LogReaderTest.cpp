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
#include <string>
#include <type_traits>

#include "Basics/Exceptions.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogMetaPayload.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/FileHeader.h"
#include "Replication2/Storage/WAL/LogReader.h"
#include "Replication2/Storage/WAL/Record.h"
#include "Replication2/Storage/WAL/RecordType.h"
#include "velocypack/Builder.h"

#include "Mocks/Death_Test.h"
#include "Replication2/Storage/InMemoryLogFile.h"
#include "Replication2/Storage/MockFileReader.h"

namespace arangodb::replication2::storage::wal::test {

using namespace testing;

namespace {

auto numPaddingBytes(std::uint64_t payloadSize) -> std::uint64_t {
  return Record::paddedPayloadSize(payloadSize) - payloadSize;
}

void appendValueToBuffer(Buffer& buffer, char const* v) {
  buffer.append(v, strlen(v));
}

void appendValueToBuffer(Buffer& buffer, VPackSlice v) {
  buffer.append(v.getDataPtr(), v.byteSize());
}

void appendValueToBuffer(Buffer& buffer, std::string const& v) {
  buffer.append(v.data(), v.size());
}

template<class T>
void appendValueToBuffer(Buffer& buffer,
                         T&& arg) requires std::is_trivially_copyable_v<T> {
  buffer.append(std::forward<T>(arg));
}

void appendToBuffer(Buffer& buffer) {}

template<class T, class... Ts>
void appendToBuffer(Buffer& buffer, T&& arg, Ts&&... args) {
  appendValueToBuffer(buffer, std::forward<T>(arg));
  appendToBuffer(buffer, std::forward<Ts>(args)...);
}

template<class... Ts>
auto createBuffer(Ts&&... args) -> std::string {
  Buffer buffer;
  appendToBuffer(buffer, std::forward<Ts>(args)...);
  std::string result(buffer.size(), '\0');
  std::memcpy(result.data(), buffer.data(), buffer.size());
  return result;
}

auto recordHeader(std::uint64_t index, std::uint64_t payloadSize,
                  RecordType type = RecordType::wNormal)
    -> Record::CompressedHeader {
  Record::Header header{};
  header.index = index;
  header.term = 1;
  header.type = type;
  header.payloadSize = payloadSize;
  return Record::CompressedHeader{header};
}

}  // namespace

TEST(LogReaderTest, create_should_throw_if_header_cannot_be_read) {
  auto buffer = createBuffer("blubb");
  try {
    LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
    FAIL();
  } catch (const basics::Exception& e) {
    EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, e.code());
    EXPECT_EQ(
        "failed to read file header from log file in-memory file - end of file "
        "reached",
        e.message());
  }
}

TEST(LogReaderTest, create_should_throw_if_magic_number_is_invalid) {
  auto buffer =
      createBuffer(FileHeader{.magic = 0, .version = wCurrentVersion});
  try {
    LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
    FAIL();
  } catch (const basics::Exception& e) {
    EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_INVALID_FILE, e.code());
    EXPECT_EQ("invalid file type in log file in-memory file", e.message());
  }
}

TEST(LogReaderTest, create_should_throw_if_file_version_is_invalid) {
  auto buffer =
      createBuffer(FileHeader{.magic = wMagicFileType, .version = 42});
  try {
    LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
    FAIL();
  } catch (const basics::Exception& e) {
    EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_INVALID_FILE, e.code());
    EXPECT_EQ("invalid file version in log file in-memory file", e.message());
  }
}

TEST(LogReaderTest, position_should_forward_to_file_reader) {
  auto fileReader = std::make_unique<MockFileReader>();
  EXPECT_CALL(*fileReader, position()).WillOnce(Return(42));
  LogReader reader(std::move(fileReader), 0);
  EXPECT_EQ(42, reader.position());
}

TEST(LogReaderTest, size_should_forward_to_file_reader) {
  auto fileReader = std::make_unique<MockFileReader>();
  EXPECT_CALL(*fileReader, size()).WillOnce(Return(42));
  LogReader reader(std::move(fileReader), 0);
  EXPECT_EQ(42, reader.size());
}

TEST(LogReaderTest, seek_forwards_to_file_reader) {
  auto fileReader = std::make_unique<MockFileReader>();
  EXPECT_CALL(*fileReader, seek(42));
  LogReader reader(std::move(fileReader), 0);
  reader.seek(42);
}

TEST(LogReaderTest, seek_prevents_seeking_before_the_first_entry) {
  auto fileReader = std::make_unique<MockFileReader>();
  EXPECT_CALL(*fileReader, seek(8));
  LogReader reader(std::move(fileReader), 8);
  reader.seek(0);
}

TEST(LogReaderTest, skipEntry_skips_over_the_current_entry) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0),  //
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(2, 12),   //
      std::string(16, 'x'),  // payload
      Record::Footer{.size = sizeof(Record::CompressedHeader) + 16 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  EXPECT_EQ(sizeof(FileHeader), reader.position());
  reader.skipEntry();
  EXPECT_EQ(sizeof(FileHeader) + sizeof(Record::CompressedHeader) +
                sizeof(Record::Footer),
            reader.position());
  reader.skipEntry();
  EXPECT_EQ(buffer.size(), reader.position());
}

TEST(LogReaderTest,
     readNextLogEntry_should_return_error_if_record_header_cannot_be_read) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion}, 42);

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.readNextLogEntry();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_END_OF_FILE, res.errorNumber());
  EXPECT_EQ("failed to read record header - end of file reached",
            res.errorMessage());
}

TEST(LogReaderTest,
     readNextLogEntry_should_return_error_if_payload_cannot_be_read) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 30), 123);

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.readNextLogEntry();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, res.errorNumber());
  EXPECT_EQ("failed to read record payload - end of file reached",
            res.errorMessage());
}

TEST(LogReaderTest,
     readNextLogEntry_should_return_error_if_record_footer_cannot_be_read) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 30), std::string(32, 'x'),  // payload
      123);

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.readNextLogEntry();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, res.errorNumber());
  EXPECT_EQ("failed to read record footer - end of file reached",
            res.errorMessage());
}

TEST(LogReaderTest,
     readNextLogEntry_should_crash_if_crc32_of_payload_is_invalid) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 30), std::string(32, 'x'),  // payload
      Record::Footer{.crc32 = 42,
                     .size = sizeof(Record::CompressedHeader) + 32 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  ASSERT_DEATH_CORE_FREE(reader.readNextLogEntry(), "");
}

TEST(LogReaderTest,
     readNextLogEntry_should_crash_if_footer_size_is_not_a_multiple_of_8) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 30), std::string(32, 'x'),  // payload
      Record::Footer{.crc32 = 1684506276,
                     .size = sizeof(Record::CompressedHeader) + 31 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  ASSERT_DEATH_CORE_FREE(reader.readNextLogEntry(), "");
}

TEST(
    LogReaderTest,
    readNextLogEntry_should_crash_if_footer_size_does_not_match_the_sum_of_header_size_padded_payload_size_and_footer_size) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 30), std::string(32, 'x'),  // payload
      Record::Footer{.crc32 = 1684506276,
                     .size = sizeof(Record::CompressedHeader) + 16 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  ASSERT_DEATH_CORE_FREE(reader.readNextLogEntry(), "");
}

TEST(LogReaderTest, readNextLogEntry_can_read_normal_entry) {
  std::string payload(30, 'x');
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, payload.size()),                              //
      payload, std::string(numPaddingBytes(payload.size()), '\0'),  // padding
      Record::Footer{.crc32 = 217008260,
                     .size = sizeof(Record::CompressedHeader) +
                             Record::paddedPayloadSize(payload.size()) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.readNextLogEntry();
  ASSERT_TRUE(res.ok());
  auto entry = res.get();
  EXPECT_EQ(sizeof(FileHeader), entry.position().fileOffset());
  EXPECT_EQ(LogIndex{2}, entry.position().index());

  EXPECT_EQ(LogIndex{2}, entry.entry().logIndex());
  EXPECT_EQ(LogTerm{1}, entry.entry().logTerm());
  auto* logPayload = entry.entry().logPayload();
  ASSERT_NE(nullptr, logPayload);
  EXPECT_EQ(payload.size(), logPayload->byteSize());
  EXPECT_TRUE(std::memcmp(payload.data(), logPayload->slice().getDataPtr(),
                          payload.size()) == 0);
}

TEST(LogReaderTest, readNextLogEntry_can_read_meta_entry) {
  auto payload = LogMetaPayload::withPing(
      "message",
      // the timepoint is serialized as seconds, so we must avoid
      // sub-second precision for simple equality comparison
      LogMetaPayload::Ping::clock::time_point{
          std::chrono::milliseconds(123000)});
  VPackBuilder builder;
  payload.toVelocyPack(builder);
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, builder.size(), RecordType::wMeta),  //
      builder.slice(),
      std::string(numPaddingBytes(builder.size()), '\0'),  // padding
      Record::Footer{.crc32 = 3763024318,
                     .size = sizeof(Record::CompressedHeader) +
                             Record::paddedPayloadSize(builder.size()) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.readNextLogEntry();
  ASSERT_TRUE(res.ok());
  auto entry = res.get();
  EXPECT_EQ(sizeof(FileHeader), entry.position().fileOffset());
  EXPECT_EQ(LogIndex{2}, entry.position().index());

  EXPECT_EQ(LogIndex{2}, entry.entry().logIndex());
  EXPECT_EQ(LogTerm{1}, entry.entry().logTerm());
  auto* meta = entry.entry().meta();
  ASSERT_NE(nullptr, meta);
  EXPECT_EQ(payload.info, meta->info);
}

TEST(LogReaderTest,
     getFirstRecordHeader_should_return_error_if_header_cannot_be_read) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion}, 42);

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getFirstRecordHeader();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, res.errorNumber());
  EXPECT_EQ("failed to read record header - end of file reached",
            res.errorMessage());
}

TEST(LogReaderTest, getFirstRecordHeader) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0),  //
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(2, 12),   //
      std::string(16, 'x'),  // payload
      Record::Footer{.size = sizeof(Record::CompressedHeader) + 16 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getFirstRecordHeader();
  ASSERT_TRUE(res.ok());
  auto header = Record::Header{res.get()};
  EXPECT_EQ(1, header.index);
  EXPECT_EQ(1, header.term);
  EXPECT_EQ(0, header.payloadSize);
}

TEST(LogReaderTest, getLastRecordHeader) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0),  //
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(2, 12),   //
      std::string(16, 'x'),  // payload
      Record::Footer{.size = sizeof(Record::CompressedHeader) + 16 +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getLastRecordHeader();
  ASSERT_TRUE(res.ok());
  auto header = Record::Header{res.get()};
  EXPECT_EQ(2, header.index);
  EXPECT_EQ(1, header.term);
  EXPECT_EQ(12, header.payloadSize);
}

TEST(LogReaderTest,
     getLastRecordHeader_should_return_error_if_file_is_too_small) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0));

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getLastRecordHeader();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT, res.errorNumber());
  EXPECT_EQ("log file in-memory file is too small", res.errorMessage());
}

TEST(
    LogReaderTest,
    getLastRecordHeader_should_return_error_if_footer_size_is_not_a_multiple_of_8) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer) + 1});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getLastRecordHeader();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT, res.errorNumber());
  EXPECT_EQ("invalid footer size in file in-memory file", res.errorMessage());
}

TEST(
    LogReaderTest,
    getLastRecordHeader_should_return_error_if_footer_size_is_greater_than_file_size_minus_file_header) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer) + 8});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getLastRecordHeader();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT, res.errorNumber());
  EXPECT_EQ("invalid footer size in file in-memory file", res.errorMessage());
}

TEST(
    LogReaderTest,
    getLastRecordHeader_should_return_error_if_footer_size_does_not_match_padded_payload_size_plus_header_size_plus_footer_size) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(1, 1),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.getLastRecordHeader();
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_CORRUPT, res.errorNumber());
  EXPECT_EQ("Mismatching size information in file in-memory file",
            res.errorMessage());
}

TEST(LogReaderTest,
     seekLogIndexForward_should_return_error_if_log_index_is_not_found) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.seekLogIndexForward(LogIndex{3});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR, res.errorNumber());
  EXPECT_EQ("log index 3 not found in file in-memory file", res.errorMessage());
}

TEST(LogReaderTest, seekLogIndexForward_should_crash_if_log_indexes_have_gaps) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(4, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  ASSERT_DEATH_CORE_FREE(reader.seekLogIndexForward(LogIndex{4}), "");
}

TEST(LogReaderTest,
     seekLogIndexForward_should_crash_if_log_indexes_are_not_sequential) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(2, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  ASSERT_DEATH_CORE_FREE(reader.seekLogIndexForward(LogIndex{4}), "");
}

TEST(
    LogReaderTest,
    seekLogIndexForward_should_position_reader_at_the_specified_log_index_and_return_its_header) {
  auto buffer = createBuffer(
      FileHeader{.magic = wMagicFileType, .version = wCurrentVersion},
      recordHeader(2, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)},
      recordHeader(3, 12),   //
      std::string(16, 'x'),  // payload
      Record::Footer{.size = sizeof(Record::CompressedHeader) + 16 +
                             sizeof(Record::Footer)},
      recordHeader(4, 0),
      Record::Footer{.size = sizeof(Record::CompressedHeader) +
                             sizeof(Record::Footer)});

  LogReader reader(std::make_unique<InMemoryFileReader>(buffer));
  auto res = reader.seekLogIndexForward(LogIndex{4});
  ASSERT_TRUE(res.ok());
  auto header = res.get();
  EXPECT_EQ(4, header.index);
  auto expectedPos =
      sizeof(FileHeader) +                                         //
      sizeof(Record::CompressedHeader) + sizeof(Record::Footer) +  //
      sizeof(Record::CompressedHeader) + 16 + sizeof(Record::Footer);
  EXPECT_EQ(expectedPos, reader.position());
}

// TODO

}  // namespace arangodb::replication2::storage::wal::test
