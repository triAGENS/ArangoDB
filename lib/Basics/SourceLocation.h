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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <iosfwd>
#include <source_location>
#include <string_view>

namespace arangodb::basics {

// This is mostly a custom type around std::source_location with the same
// interface; However, it changes the filename's path, so it's relative to the
// git root directory.
struct SourceLocation {
 private:
  char const* _fileName{};
  char const* _functionName{};
  std::uint_least32_t _line{};
  std::uint_least32_t _column{};

 public:
  consteval static auto current(
      std::source_location loc = std::source_location::current())
      -> SourceLocation {
    return SourceLocation{loc};
  }

  // We can allow this if we need to, but this way it prevents accidental usage
  // of uninitialized SourceLocations.
  SourceLocation() = delete;

  consteval explicit SourceLocation(std::source_location location) noexcept
      : _fileName(_stripPrefix(location.file_name())),
        _functionName(location.function_name()),
        _line(location.line()) {}

  [[nodiscard]] constexpr auto file_name() const noexcept
      -> decltype(_fileName) {
    return _fileName;
  }

  [[nodiscard]] constexpr auto line() const noexcept -> decltype(_line) {
    return _line;
  }

  [[nodiscard]] constexpr auto function_name() const noexcept
      -> decltype(_functionName) {
    return _functionName;
  }

  [[nodiscard]] constexpr auto column() const noexcept -> decltype(_column) {
    return _column;
  }

  // if pathStr is `/path/to/arangodb/lib/Basics/SourceLocation.h`,
  // then this will return `lib/Basics/SourceLocation.h`
  consteval static auto _stripPrefix(const char* pathStr) -> const char*;
};

auto operator<<(std::ostream&, SourceLocation const&) -> std::ostream&;

auto to_string(SourceLocation const&) -> std::string;

// if pathStr is `/path/to/arangodb/lib/Basics/SourceLocation.h`,
// then this will return `lib/Basics/SourceLocation.h`
consteval auto SourceLocation::_stripPrefix(const char* pathStr) -> const
    char* {
  // `/path/to/arangodb/lib/Basics/SourceLocation.h`
  constexpr auto thisFile = std::string_view(__FILE__);
  // `/path/to/arangodb/lib/Basics`
  constexpr auto thisDir = thisFile.substr(0, thisFile.rfind('/'));
  // `/path/to/arangodb/lib`
  constexpr auto libDir = thisDir.substr(0, thisDir.rfind('/'));
  // `/path/to/arangodb`
  constexpr auto srcDir = libDir.substr(0, libDir.rfind('/'));
  auto path = std::string_view{pathStr};
  if (path.starts_with(srcDir)) {
    // + 1 removes the slash
    path.remove_prefix(srcDir.size() + 1);
  }
  return path.data();
}

static_assert(std::string_view{"lib/Basics/SourceLocation.h"}.compare(
                  SourceLocation::_stripPrefix(__FILE__)) == 0);

}  // namespace arangodb::basics

#define ADB_HERE (::arangodb::basics::SourceLocation::current())
