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

#include "FileWriterImpl.h"

#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "Assertions/ProdAssert.h"
#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"
#include "Logger/LogMacros.h"
#include "Replication2/Storage/WAL/FileReaderImpl.h"

namespace arangodb::replication2::storage::wal {
#ifndef _WIN32
FileWriterImplPosix::FileWriterImplPosix(std::filesystem::path path)
    : _path(std::move(path)) {
  _file = ::open(_path.c_str(), O_CREAT | O_RDWR | O_APPEND,
                 S_IRUSR | S_IWUSR | O_CLOEXEC);
  ADB_PROD_ASSERT(_file >= 0)
      << "failed to open replicated log file" << _path.string()
      << " for writing with error " << strerror(errno);
  auto off = ::lseek(_file, 0, SEEK_END);
  ADB_PROD_ASSERT(off >= 0)
      << "failed to obtain file size for file " << _path.string()
      << " with error " << strerror(errno);
  _size = off;

  // we also need to fsync the directory to ensure that the file is visible!
  auto dir = _path.parent_path();
  auto fd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
  ADB_PROD_ASSERT(fd >= 0) << "failed to open directory " << dir.string()
                           << " with error " << strerror(errno);
  ADB_PROD_ASSERT(fsync(fd) == 0)
      << "failed to fsync directory " << dir.string() << " with error "
      << strerror(errno);
}

FileWriterImplPosix::~FileWriterImplPosix() {
  if (_file >= 0) {
    sync();
    ADB_PROD_ASSERT(::close(_file) == 0)
        << "failed to close replicated log file " << _path.string()
        << " with error " << strerror(errno);
  }
}

auto FileWriterImplPosix::append(std::string_view data) -> Result {
  auto n = ::write(_file, data.data(), data.size());
  ADB_PROD_ASSERT(n >= 0) << "failed to write to log file " + _path.string() +
                                 ": " + strerror(errno);

  if (static_cast<std::size_t>(n) != data.size()) {
    // try to revert the partial write. this is only best effort - we have to
    // abort anyway!
    std::ignore = ::ftruncate(_file, _size);
    ADB_PROD_ASSERT(false) << "write to log file " << _path.string()
                           << " was incomplete; could only write " << n
                           << " of " << data.size() << " bytes - "
                           << strerror(errno);
  }

  _size += n;
  return Result{};
}

void FileWriterImplPosix::truncate(std::uint64_t size) {
  ADB_PROD_ASSERT(::ftruncate(_file, size) == 0)
      << "failed to truncate file" << _path.string() << " to size " << size
      << ": " << strerror(errno);
}

void FileWriterImplPosix::sync() {
  ADB_PROD_ASSERT(fdatasync(_file) == 0)
      << "failed to flush file " << _path.string() << ": " << strerror(errno);
}

auto FileWriterImplPosix::size() const -> std::uint64_t {
  // note - we do not need to check the current position since the file is
  // append only, so we always write at the end anyway
  auto res = lseek(_file, 0, SEEK_END);
  ADB_PROD_ASSERT(res >= 0);
  return static_cast<std::uint64_t>(res);
}

auto FileWriterImplPosix::getReader() const -> std::unique_ptr<IFileReader> {
  return std::make_unique<FileReaderImpl>(_path);
}

#else

FileWriterImplWindows::FileWriterImplWindows(std::filesystem::path path)
    : _path(std::move(path)) {
  _file = CreateFileW(_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  ADB_PROD_ASSERT(_file != INVALID_HANDLE_VALUE)
      << "failed to open replicated log file " << path.string()
      << " for writing with error " << GetLastError();
}

FileWriterImplWindows::~FileWriterImplWindows() {
  if (_file != INVALID_HANDLE_VALUE) {
    CloseHandle(_file);
  }
}

auto FileWriterImplWindows::append(std::string_view data) -> Result {
  DWORD written = 0;
  TRI_ASSERT(data.size() < std::numeric_limits<DWORD>::max());
  if (WriteFile(_file, data.data(), static_cast<DWORD>(data.size()), &written,
                nullptr) == FALSE) {
    return Result(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
                  "failed to write to log file " + _path.string() + ": " +
                      std::to_string(GetLastError()));
  }
  if (static_cast<std::size_t>(written) != data.size()) {
    return Result(TRI_ERROR_REPLICATION_REPLICATED_WAL_ERROR,
                  "write to log file was incomplete");
  }

  return Result{};
}

void FileWriterImplWindows::truncate(std::uint64_t size) {
  TRI_ASSERT(size < std::numeric_limits<DWORD>::max());
  ADB_PROD_ASSERT(SetFilePointer(_file, static_cast<DWORD>(size), nullptr,
                                 FILE_BEGIN) != INVALID_SET_FILE_POINTER)
      << "failed to set file" << _path.string() << " to position" << size
      << ": " << GetLastError();
  ADB_PROD_ASSERT(SetEndOfFile(_file))
      << "failed to truncate file" << _path.string() << " to size " << size
      << ": " << GetLastError();
}

void FileWriterImplWindows::sync() {
  ADB_PROD_ASSERT(FlushFileBuffers(_file))
      << "failed to flush file " << _path.string() << ": " << GetLastError();
}

auto FileWriterImplWindows::size() const -> std::uint64_t {
  LARGE_INTEGER size;
  auto res = GetFileSizeEx(_file, &size);
  ADB_PROD_ASSERT(res) << "Failed to get size of file " << _path.string();
  return size.QuadPart;
}

auto FileWriterImplWindows::getReader() const -> std::unique_ptr<IFileReader> {
  return std::make_unique<FileReaderImpl>(_path.string());
}

#endif
}  // namespace arangodb::replication2::storage::wal
