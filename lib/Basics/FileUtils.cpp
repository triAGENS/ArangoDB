////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include "FileUtils.h"

#include "Basics/process-utils.h"
#include "Basics/NumberUtils.h"

#ifdef TRI_HAVE_DIRENT_H
#include <dirent.h>
#endif

#ifdef TRI_HAVE_DIRECT_H
#include <direct.h>
#endif
#ifdef TRI_HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>

#include "Basics/Exceptions.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/debugging.h"
#include "Basics/error.h"
#include "Basics/files.h"
#include "Basics/voc-errors.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"

namespace {
std::function<bool(std::string const&)> const passAllFilter =
    [](std::string const&) { return false; };

enum class StatResultType {
  Error,  // in case it cannot be determined
  Directory,
  SymLink,
  File,
  Other  // potentially file
};

StatResultType statResultType(TRI_stat_t const& stbuf) {
  if (S_ISDIR(stbuf.st_mode)) {
    return StatResultType::Directory;
  }

  if (S_ISLNK(stbuf.st_mode)) {
    return StatResultType::SymLink;
  }

  if ((stbuf.st_mode & S_IFMT) == S_IFREG) {
    return StatResultType::File;
  }

  return StatResultType::Other;
}

StatResultType statResultType(std::string const& path) {
  TRI_stat_t stbuf;
  int res = TRI_STAT(path.c_str(), &stbuf);
  if (res != 0) {
    return StatResultType::Error;
  }
  return statResultType(stbuf);
}

void processFiles(std::string const& directory,
                  std::function<void(std::string const&)> const& cb) {
  DIR* d = opendir(directory.c_str());

  if (d == nullptr) {
    auto res = TRI_set_errno(TRI_ERROR_SYS_ERROR);

    auto message = arangodb::basics::StringUtils::concatT(
        "failed to enumerate files in directory '", directory,
        "': ", TRI_last_error());
    THROW_ARANGO_EXCEPTION_MESSAGE(res, std::move(message));
  }

  auto guard = arangodb::scopeGuard([&]() noexcept { closedir(d); });

  std::string rcs;
  dirent* de = readdir(d);

  while (de != nullptr) {
    // stringify filename (convert to std::string). we take this performance
    // hit because we will need to strlen anyway and need to pass it to a
    // function that will likely use its stringified value anyway
    rcs.assign(de->d_name);

    if (rcs != "." && rcs != "..") {
      // run callback function
      cb(rcs);
    }

    // advance to next entry
    de = readdir(d);
  }
}

}  // namespace

namespace arangodb::basics::FileUtils {

////////////////////////////////////////////////////////////////////////////////
/// @brief removes trailing path separators from path
///
/// path will be modified in-place
////////////////////////////////////////////////////////////////////////////////

std::string removeTrailingSeparator(std::string const& name) {
  size_t endpos = name.find_last_not_of(TRI_DIR_SEPARATOR_CHAR);
  if (endpos != std::string::npos) {
    return name.substr(0, endpos + 1);
  }

  return name;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief normalizes path
///
/// path will be modified in-place
////////////////////////////////////////////////////////////////////////////////

void normalizePath(std::string& name) {
  std::replace(name.begin(), name.end(), '/', TRI_DIR_SEPARATOR_CHAR);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a filename
////////////////////////////////////////////////////////////////////////////////

std::string buildFilename(char const* path, char const* name) {
  TRI_ASSERT(path != nullptr);
  TRI_ASSERT(name != nullptr);

  std::string result(path);

  if (!result.empty()) {
    result = removeTrailingSeparator(result);
    if (result.length() != 1 || result[0] != TRI_DIR_SEPARATOR_CHAR) {
      result += TRI_DIR_SEPARATOR_CHAR;
    }
  }

  if (!result.empty() && *name == TRI_DIR_SEPARATOR_CHAR) {
    // skip initial forward slash in name to avoid having two forward slashes in
    // result
    result.append(name + 1);
  } else {
    result.append(name);
  }
  normalizePath(result);  // in place

  return result;
}

std::string buildFilename(std::string const& path, std::string const& name) {
  std::string result(path);

  if (!result.empty()) {
    result = removeTrailingSeparator(result);
    if (result.length() != 1 || result[0] != TRI_DIR_SEPARATOR_CHAR) {
      result += TRI_DIR_SEPARATOR_CHAR;
    }
  }

  if (!result.empty() && !name.empty() && name[0] == TRI_DIR_SEPARATOR_CHAR) {
    // skip initial forward slash in name to avoid having two forward slashes in
    // result
    result.append(name.c_str() + 1, name.size() - 1);
  } else {
    result.append(name);
  }
  normalizePath(result);  // in place

  return result;
}

static void throwFileReadError(std::string const& filename) {
  TRI_set_errno(TRI_ERROR_SYS_ERROR);

  auto message = StringUtils::concatT("read failed for file '", filename,
                                      "': ", TRI_last_error());
  LOG_TOPIC("a0898", TRACE, arangodb::Logger::FIXME) << message;

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SYS_ERROR, std::move(message));
}

static void throwFileWriteError(std::string const& filename) {
  TRI_set_errno(TRI_ERROR_SYS_ERROR);

  auto message = StringUtils::concatT("write failed for file '", filename,
                                      "': ", TRI_last_error());
  LOG_TOPIC("a8930", TRACE, arangodb::Logger::FIXME) << "" << message;

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SYS_ERROR, message);
}

static void throwFileCreateError(std::string const& filename) {
  TRI_set_errno(TRI_ERROR_SYS_ERROR);

  auto message = StringUtils::concatT("failed to create file '", filename,
                                      "': ", TRI_last_error());
  LOG_TOPIC("208ba", TRACE, arangodb::Logger::FIXME) << "" << message;

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SYS_ERROR, message);
}

static void fillString(int fd, std::string const& filename, size_t filesize,
                       std::string& result) {
  constexpr size_t chunkSize = 8192;

  result.clear();
  result.reserve(filesize + 2 * chunkSize);

  size_t pos = 0;

  while (true) {
    result.resize(result.size() + chunkSize);

    TRI_read_return_t n =
        TRI_READ(fd, result.data() + pos, static_cast<TRI_read_t>(chunkSize));

    if (n == 0) {
      break;
    }

    if (n < 0) {
      throwFileReadError(filename);
    }

    pos += static_cast<size_t>(n);
  }

  result.resize(pos);
}

void slurp(std::string const& filename, std::string& result) {
  int64_t filesize = TRI_SizeFile(filename.c_str());
  if (filesize < 0) {
    throwFileReadError(filename);
  }

  int fd = TRI_OPEN(filename.c_str(), O_RDONLY | TRI_O_CLOEXEC);

  if (fd == -1) {
    throwFileReadError(filename);
  }

  auto sg = arangodb::scopeGuard([&]() noexcept { TRI_CLOSE(fd); });

  fillString(fd, filename, static_cast<size_t>(filesize), result);
}

std::string slurp(std::string const& filename) {
  std::string result;
  slurp(filename, result);
  return result;
}

void spit(std::string const& filename, char const* ptr, size_t len, bool sync) {
  int fd =
      TRI_CREATE(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | TRI_O_CLOEXEC,
                 S_IRUSR | S_IWUSR | S_IRGRP);

  if (fd == -1) {
    throwFileCreateError(filename);
  }

  auto sg = arangodb::scopeGuard([&]() noexcept { TRI_CLOSE(fd); });

  while (0 < len) {
    auto n = TRI_WRITE(fd, ptr, static_cast<TRI_write_t>(len));

    if (n < 0) {
      throwFileWriteError(filename);
    }

    ptr += n;
    len -= n;
  }

  if (sync) {
    // intentionally ignore this error as there is nothing we can do about it
    TRI_fsync(fd);
  }
}

void spit(std::string const& filename, std::string const& content, bool sync) {
  spit(filename, content.data(), content.size(), sync);
}

void appendToFile(std::string const& filename, char const* ptr, size_t len,
                  bool sync) {
  int fd = TRI_OPEN(filename.c_str(), O_WRONLY | O_APPEND | TRI_O_CLOEXEC);

  if (fd == -1) {
    throwFileWriteError(filename);
  }

  auto sg = arangodb::scopeGuard([&]() noexcept { TRI_CLOSE(fd); });

  while (0 < len) {
    auto n = TRI_WRITE(fd, ptr, static_cast<TRI_write_t>(len));

    if (n < 0) {
      throwFileWriteError(filename);
    }

    ptr += n;
    len -= n;
  }

  if (sync) {
    // intentionally ignore this error as there is nothing we can do about it
    TRI_fsync(fd);
  }
}

void appendToFile(std::string const& filename, std::string_view s, bool sync) {
  appendToFile(filename, s.data(), s.size(), sync);
}

ErrorCode remove(std::string const& fileName) {
  auto const success = 0 == std::remove(fileName.c_str());

  if (!success) {
    return TRI_set_errno(TRI_ERROR_SYS_ERROR);
  }

  return TRI_ERROR_NO_ERROR;
}

bool createDirectory(std::string const& name, ErrorCode* errorNumber) {
  if (errorNumber != nullptr) {
    *errorNumber = TRI_ERROR_NO_ERROR;
  }

  return createDirectory(name, 0777, errorNumber);
}

bool createDirectory(std::string const& name, int mask,
                     ErrorCode* errorNumber) {
  if (errorNumber != nullptr) {
    *errorNumber = TRI_ERROR_NO_ERROR;
  }

  auto result = TRI_MKDIR(name.c_str(), static_cast<mode_t>(mask));

  if (result != 0) {
    int res = errno;
    if (res == EEXIST && isDirectory(name)) {
      result = 0;
    } else {
      auto errorCode = TRI_set_errno(TRI_ERROR_SYS_ERROR);
      if (errorNumber != nullptr) {
        *errorNumber = errorCode;
      }
    }
  }

  return result == 0;
}

/// @brief will not copy files/directories for which the filter function
/// returns true (now wrapper for version below with TRI_copy_recursive_e
/// filter)
bool copyRecursive(std::string const& source, std::string const& target,
                   std::function<bool(std::string const&)> const& filter,
                   std::string& error) {
  // "auto lambda" will not work here
  std::function<TRI_copy_recursive_e(std::string const&)> lambda =
      [&filter](std::string const& pathname) -> TRI_copy_recursive_e {
    return filter(pathname) ? TRI_COPY_IGNORE : TRI_COPY_COPY;
  };

  return copyRecursive(source, target, lambda, error);

}  // copyRecursive (bool filter())

/// @brief will not copy files/directories for which the filter function
/// returns true
bool copyRecursive(
    std::string const& source, std::string const& target,
    std::function<TRI_copy_recursive_e(std::string const&)> const& filter,
    std::string& error) {
  if (isDirectory(source)) {
    return copyDirectoryRecursive(source, target, filter, error);
  }

  switch (filter(source)) {
    case TRI_COPY_IGNORE:
      return true;  // original TRI_ERROR_NO_ERROR implies "false", seems wrong

    case TRI_COPY_COPY:
      return TRI_CopyFile(source, target, error);

    case TRI_COPY_LINK:
      return TRI_CreateHardlink(source, target, error);

    default:
      return false;  // TRI_ERROR_BAD_PARAMETER seems wrong since returns "true"
  }
}

/// @brief will not copy files/directories for which the filter function
/// returns true
bool copyDirectoryRecursive(
    std::string const& source, std::string const& target,
    std::function<TRI_copy_recursive_e(std::string const&)> const& filter,
    std::string& error) {
  bool rc_bool = true;

  // these strings will be recycled over and over
  std::string dst = target + TRI_DIR_SEPARATOR_STR;
  size_t const dstPrefixLength = dst.size();
  std::string src = source + TRI_DIR_SEPARATOR_STR;
  size_t const srcPrefixLength = src.size();

  DIR* filedir = opendir(source.c_str());

  if (filedir == nullptr) {
    error = "directory " + source + " not found";
    return false;
  }

  struct dirent* oneItem = nullptr;

  // do not use readdir_r() here anymore as it is not safe and deprecated
  // in newer versions of libc:
  // http://man7.org/linux/man-pages/man3/readdir_r.3.html
  // the man page recommends to use plain readdir() because it can be expected
  // to be thread-safe in reality, and newer versions of POSIX may require its
  // thread-safety formally, and in addition obsolete readdir_r() altogether
  while ((oneItem = (readdir(filedir))) != nullptr && rc_bool) {
    char const* fn = oneItem->d_name;

    // Now iterate over the items.
    // check its not the pointer to the upper directory:
    if (!strcmp(fn, ".") || !strcmp(fn, "..")) {
      continue;
    }

    // add current filename to prefix
    src.resize(srcPrefixLength);
    TRI_ASSERT(src.back() == TRI_DIR_SEPARATOR_CHAR);
    src.append(fn);

    auto filterResult = filter(src);

    if (filterResult != TRI_COPY_IGNORE) {
      // prepare dst filename
      dst.resize(dstPrefixLength);
      TRI_ASSERT(dst.back() == TRI_DIR_SEPARATOR_CHAR);
      dst.append(fn);

      // figure out the type of the directory entry.
      StatResultType type = StatResultType::Error;
      TRI_stat_t stbuf;
      int res = TRI_STAT(src.c_str(), &stbuf);
      if (res == 0) {
        type = ::statResultType(stbuf);
      }

      switch (filterResult) {
        case TRI_COPY_IGNORE:
          TRI_ASSERT(false);
          break;

        case TRI_COPY_COPY:
          // Handle subdirectories:
          if (type == StatResultType::Directory) {
            long systemError;
            auto rc = TRI_CreateDirectory(dst.c_str(), systemError, error);
            if (rc != TRI_ERROR_NO_ERROR && rc != TRI_ERROR_FILE_EXISTS) {
              rc_bool = false;
              break;
            }
            if (!copyDirectoryRecursive(src, dst, filter, error)) {
              rc_bool = false;
              break;
            }
            if (!TRI_CopyAttributes(src, dst, error)) {
              rc_bool = false;
              break;
            }
          } else if (type == StatResultType::SymLink) {
            if (!TRI_CopySymlink(src, dst, error)) {
              rc_bool = false;
            }
          } else {
            // optimized version that reuses the already retrieved stat data
            rc_bool = TRI_CopyFile(src, dst, error, &stbuf);
          }
          break;

        case TRI_COPY_LINK:
          if (!TRI_CreateHardlink(src, dst, error)) {
            rc_bool = false;
          }  // if
          break;
      }  // switch
    }
  }
  closedir(filedir);

  return rc_bool;
}

std::vector<std::string> listFiles(std::string const& directory) {
  std::vector<std::string> result;

  ::processFiles(directory, [&result](std::string const& filename) {
    result.push_back(filename);
  });

  return result;
}

size_t countFiles(std::string const& directory) {
  size_t result = 0;

  ::processFiles(directory,
                 [&result](std::string const& filename) { ++result; });

  return result;
}

bool isDirectory(std::string const& path) {
  return ::statResultType(path) == ::StatResultType::Directory;
}

bool isSymbolicLink(std::string const& path) {
  return ::statResultType(path) == ::StatResultType::SymLink;
}

bool isRegularFile(std::string const& path) {
  return ::statResultType(path) == ::StatResultType::File;
}

bool exists(std::string const& path) {
  return ::statResultType(path) != ::StatResultType::Error;
}

off_t size(std::string const& path) {
  int64_t result = TRI_SizeFile(path.c_str());

  if (result < 0) {
    return (off_t)0;
  }

  return (off_t)result;
}

std::string stripExtension(std::string const& path,
                           std::string const& extension) {
  size_t pos = path.rfind(extension);
  if (pos == std::string::npos) {
    return path;
  }

  std::string last = path.substr(pos);
  if (last == extension) {
    return path.substr(0, pos);
  }

  return path;
}

FileResult changeDirectory(std::string const& path) {
  int res = TRI_CHDIR(path.c_str());

  if (res == 0) {
    return FileResult();
  } else {
    return FileResult(errno);
  }
}

FileResultString currentDirectory() {
  size_t len = 1000;
  std::unique_ptr<char[]> current(new char[len]);

  while (TRI_GETCWD(current.get(), (int)len) == nullptr) {
    if (errno == ERANGE) {
      len += 1000;
      current.reset(new char[len]);
    } else {
      return FileResultString(errno, ".");
    }
  }

  std::string result = current.get();

  return FileResultString(result);
}

std::string homeDirectory() { return TRI_HomeDirectory(); }

std::string configDirectory(char const* binaryPath) {
  std::string dir = TRI_LocateConfigDirectory(binaryPath);

  if (dir.empty()) {
    return currentDirectory().result();
  }

  return dir;
}

std::string dirname(std::string const& name) { return TRI_Dirname(name); }

void makePathAbsolute(std::string& path) {
  std::string cwd = FileUtils::currentDirectory().result();

  if (path.empty()) {
    path = cwd;
  } else {
    std::string p = TRI_GetAbsolutePath(path, cwd);
    if (!p.empty()) {
      path = p;
    }
  }
}

namespace {

// Check if path is safe (no relative paths, no special characters)
bool isPathSafe(std::string const& path) {
  // Reject empty paths
  if (path.empty()) {
    return false;
  }

  // Must be absolute path
  if (path[0] != '/') {
    return false;
  }

  // Check for dangerous characters
  static const std::string dangerousChars = "|;&$<>\"'`\\";
  if (path.find_first_of(dangerousChars) != std::string::npos) {
    return false;
  }

  // No relative path components
  if (path.find("..") != std::string::npos) {
    return false;
  }

  return true;
}

// Check if argument is safe 
bool isArgumentSafe(std::string const& arg) {
  if (arg.empty()) {
    return false;
  }

  // Check for dangerous characters
  static const std::string dangerousChars = "|;&$<>\"'`\\";
  if (arg.find_first_of(dangerousChars) != std::string::npos) {
    return false;
  }

  return true;
}

// Add allowed programs whitelist
const std::unordered_set<std::string> ALLOWED_PROGRAMS = {
  "/usr/bin/id",
  "/usr/bin/getent",
  "/usr/bin/groups"
};

std::string slurpProgramInternal(std::string const& program,
                                std::vector<std::string> const& moreArgs) {
  // Validate program path
  if (!isPathSafe(program)) {
    LOG_TOPIC("security", WARN, arangodb::Logger::FIXME)
      << "Rejected unsafe program path: " << program;
    THROW_ARANGO_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  // Check against whitelist
  if (ALLOWED_PROGRAMS.find(program) == ALLOWED_PROGRAMS.end()) {
    LOG_TOPIC("security", WARN, arangodb::Logger::FIXME)
      << "Program not in whitelist: " << program;
    THROW_ARANGO_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  // Validate all arguments
  for (auto const& arg : moreArgs) {
    if (!isArgumentSafe(arg)) {
      LOG_TOPIC("security", WARN, arangodb::Logger::FIXME)
        << "Rejected unsafe argument: " << arg;
      THROW_ARANGO_EXCEPTION(TRI_ERROR_FORBIDDEN);
    }
  }

  ExternalProcess const* process;
  ExternalId external;
  ExternalProcessStatus res;
  std::string output;
  std::vector<std::string> additionalEnv;
  char buf[1024];

  TRI_CreateExternalProcess(program.c_str(), moreArgs, additionalEnv, true,
                            &external);
  if (external._pid == TRI_INVALID_PROCESS_ID) {
    auto res = TRI_set_errno(TRI_ERROR_SYS_ERROR);

    LOG_TOPIC("a557b", TRACE, arangodb::Logger::FIXME) << StringUtils::concatT(
        "open failed for file '", program, "': ", TRI_last_error());
    THROW_ARANGO_EXCEPTION(res);
  }
  process = TRI_LookupSpawnedProcess(external._pid);
  if (process == nullptr) {
    auto res = TRI_set_errno(TRI_ERROR_SYS_ERROR);

    LOG_TOPIC("a557c", TRACE, arangodb::Logger::FIXME) << StringUtils::concatT(
        "process gone? '", program, "': ", TRI_last_error());
    THROW_ARANGO_EXCEPTION(res);
  }
  bool error = false;
  while (true) {
    auto nRead = TRI_ReadPipe(process, buf, sizeof(buf));
    if (nRead <= 0) {
      if (nRead < 0) {
        error = true;
      }
      break;
    }
    output.append(buf, nRead);
  }
  res = TRI_CheckExternalProcess(external, true, 0, noDeadLine);
  if (error) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SYS_ERROR);
  }
  // Note that we intentionally ignore the exit code of the sub process here
  // since we have always done so and do not want to break things.
  return output;
}

}  // namespace

std::string slurpProgram(std::string const& program) {
  std::vector<std::string> moreArgs{std::string("version")};
  return slurpProgramInternal(program, moreArgs);
}

#ifdef ARANGODB_HAVE_GETPWUID
std::optional<uid_t> findUser(std::string const& nameOrId) noexcept {
  // We avoid getpwuid and getpwnam because they pose problems when
  // we build static binaries with glibc (because of /etc/nsswitch.conf).
  // However, we know that `id` exists for basically all Linux variants
  // and for Mac.
  try {
    std::vector<std::string> args{"-u", nameOrId};
    std::string output = slurpProgramInternal("/usr/bin/id", args);
    StringUtils::trimInPlace(output);
    bool valid = false;
    uid_t uidNumber = NumberUtils::atoi_positive<int>(
        output.data(), output.data() + output.size(), valid);
    if (valid) {
      return {uidNumber};
    }
  } catch (std::exception const&) {
  }
  return {std::nullopt};
}

std::optional<std::string> findUserName(uid_t id) noexcept {
  // For Linux (and other Unixes), we avoid this function because it
  // poses problems when we build static binaries with glibc (because of
  // /etc/nsswitch.conf).
  try {
    std::vector<std::string> args{"passwd", std::to_string(id)};
    std::string output = slurpProgramInternal("/usr/bin/getent", args);
    StringUtils::trimInPlace(output);
    auto parts = StringUtils::split(output, ':');
    if (parts.size() >= 1) {
      return {std::move(parts[0])};
    }
  } catch (std::exception const&) {
  }
  return {std::nullopt};
}
#endif

#ifdef ARANGODB_HAVE_GETGRGID
std::optional<gid_t> findGroup(std::string const& nameOrId) noexcept {
  // For Linux (and other Unixes), we avoid these functions because they
  // pose problems when we build static binaries with glibc (because of
  // /etc/nsswitch.conf).
  try {
    std::vector<std::string> args{"group", nameOrId};
    std::string output = slurpProgramInternal("/usr/bin/getent", args);
    StringUtils::trimInPlace(output);
    auto parts = StringUtils::split(output, ':');
    if (parts.size() >= 3) {
      bool valid = false;
      uid_t gidNumber = NumberUtils::atoi_positive<int>(
          parts[2].data(), parts[2].data() + parts[2].size(), valid);
      if (valid) {
        return {gidNumber};
      }
    }
  } catch (std::exception const&) {
  }
  return {std::nullopt};
}
#endif

#ifdef ARANGODB_HAVE_INITGROUPS
void initGroups(std::string const& userName, gid_t groupId) noexcept {
  // For Linux, calling initgroups poses problems with statically linked
  // binaries, since /etc/nsswitch.conf can then lead to crashes on
  // older Linux distributions. Therefore, we need to do the groups lookup
  // ourselves using the groups command. Then we can use setgroups to
  // achieve the desired result.
  try {
    std::vector<std::string> args{userName};
    std::string output = slurpProgramInternal("/usr/bin/groups", args);
    StringUtils::trimInPlace(output);
    auto pos = output.find(':');
    if (pos != std::string::npos) {
      output = output.substr(pos + 1);
    }
    auto parts = StringUtils::split(output, ' ');
    std::vector<gid_t> groupIds{groupId};
    for (auto const& part : parts) {
      std::optional<gid_t> gidNumber = findGroup(part);
      if (gidNumber && gidNumber.value() != groupId) {
        groupIds.push_back(gidNumber.value());
      }
    }
    setgroups(groupIds.size(), groupIds.data());
  } catch (std::exception const&) {
  }
}
#endif
}  // namespace arangodb::basics::FileUtils
