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

#include "FileReaderImpl.h"
#include <cerrno>
#include <cstring>

#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"
#include "Logger/LogMacros.h"

namespace arangodb::replication2::storage::wal {

FileReaderImpl::FileReaderImpl(std::string path) : _path(std::move(path)) {
  _file = std::fopen(_path.c_str(), "rb");
  if (_file == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
                                   "failed to open replicated log file " +
                                       _path + " for reading " +
                                       strerror(errno));
  }
  setvbuf(_file, NULL, _IOFBF, 1024 * 1024);
}

FileReaderImpl::~FileReaderImpl() {
  if (_file) {
    std::fclose(_file);
  }
}

auto FileReaderImpl::read(void* buffer, std::size_t n) -> Result {
  auto numRead = std::fread(buffer, 1, n, _file);
  if (numRead != n) {
    if (std::feof(_file)) {
      return Result{TRI_ERROR_END_OF_FILE, "end of file reached"};
    }
    if (std::ferror(_file)) {
      return Result{TRI_ERROR_CANNOT_READ_FILE,
                    std::string("error reading file: ") + strerror(errno)};
    }
  }
  return {};
}

void FileReaderImpl::seek(std::uint64_t pos) {
  TRI_ASSERT(pos < std::numeric_limits<long>::max());
  ADB_PROD_ASSERT(pos <= size()) << "position " << pos << "; size " << size();
  ADB_PROD_ASSERT(std::fseek(_file, static_cast<long>(pos), SEEK_SET) == 0);
}

auto FileReaderImpl::position() const -> std::uint64_t {
  auto res = std::ftell(_file);
  ADB_PROD_ASSERT(res >= 0);
  return res;
}

auto FileReaderImpl::size() const -> std::uint64_t {
  auto const pos = std::ftell(_file);
  ADB_PROD_ASSERT(std::fseek(_file, 0, SEEK_END) == 0);
  auto const size = std::ftell(_file);
  ADB_PROD_ASSERT(std::fseek(_file, pos, SEEK_SET) == 0);
  return size;
}

}  // namespace arangodb::replication2::storage::wal
