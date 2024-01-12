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

#include "EntryWriter.h"

#include "Replication2/ReplicatedLog/LogEntry.h"
#include "Replication2/Storage/WAL/Buffer.h"
#include "Replication2/Storage/WAL/Record.h"

#include <absl/crc/crc32c.h>

namespace arangodb::replication2::storage::wal {

EntryWriter::EntryWriter(Buffer& buffer) : _buffer(buffer) {}

void EntryWriter::appendEntry(LogEntry const& entry) {
  auto startPos = _buffer.size();

  auto payloadSize = [&]() {
    if (entry.hasPayload()) {
      return writeNormalEntry(entry);
    } else {
      return writeMetaEntry(entry);
    }
  }();

  // we want every thing to be 8 byte aligned, so we round size to the next
  // greater multiple of 8
  writePaddingBytes(payloadSize);
  writeFooter(startPos);
  TRI_ASSERT(_buffer.size() % 8 == 0);
}

auto EntryWriter::writeNormalEntry(LogEntry const& entry) -> std::uint64_t {
  TRI_ASSERT(entry.hasPayload());
  Record::Header header;
  header.index = entry.logIndex().value;
  header.term = entry.logTerm().value;
  header.type = RecordType::wNormal;

  auto* payload = entry.logPayload();
  TRI_ASSERT(payload != nullptr);
  header.payloadSize = payload->byteSize();
  _buffer.append(Record::CompressedHeader{header});
  _buffer.append(payload->slice().getDataPtr(), payload->byteSize());
  return header.payloadSize;
}

auto EntryWriter::writeMetaEntry(LogEntry const& entry) -> std::uint64_t {
  TRI_ASSERT(entry.hasMeta());
  Record::Header header;
  header.index = entry.logIndex().value;
  header.term = entry.logTerm().value;
  header.type = RecordType::wMeta;
  header.payloadSize = 0;  // we initialize this to zero so the compiler is
                           // happy, but will update it later

  // the meta entry is directly encoded into the buffer, so we have to write
  // the header beforehand and update the size afterwards
  _buffer.append(Record::CompressedHeader{header});
  auto pos = _buffer.size() - sizeof(Record::CompressedHeader::payloadSize);

  auto* payload = entry.meta();
  TRI_ASSERT(payload != nullptr);
  VPackBuilder builder(_buffer.buffer());
  payload->toVelocyPack(builder);
  header.payloadSize =
      _buffer.size() - pos - sizeof(Record::CompressedHeader::payloadSize);

  // reset to the saved position so we can write the actual size
  _buffer.resetTo(pos);
  _buffer.append(static_cast<decltype(Record::CompressedHeader::payloadSize)>(
      header.payloadSize));
  _buffer.advance(header.payloadSize);
  return header.payloadSize;
}

void EntryWriter::writePaddingBytes(
    std::uint32_t payloadSize) {  // write zeroed out padding bytes
  auto numPaddingBytes = Record::paddedPayloadSize(payloadSize) - payloadSize;
  TRI_ASSERT(numPaddingBytes < 8);
  std::uint64_t const zero = 0;
  _buffer.append(&zero, numPaddingBytes);
}

void EntryWriter::writeFooter(std::size_t startPos) {
  auto size = _buffer.size() - startPos;
  Record::Footer footer{
      .crc32 = static_cast<std::uint32_t>(absl::ComputeCrc32c(
          {reinterpret_cast<char const*>(_buffer.data() + startPos), size})),
      .size = static_cast<std::uint32_t>(size + sizeof(Record::Footer))};
  TRI_ASSERT(footer.size % 8 == 0);
  _buffer.append(footer);
}

}  // namespace arangodb::replication2::storage::wal
