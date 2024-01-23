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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <iosfwd>
#include <velocypack/Slice.h>
#include <velocypack/Value.h>

namespace arangodb::replication2 {

struct LogTerm {
  constexpr LogTerm() noexcept : value{0} {}
  constexpr explicit LogTerm(std::uint64_t value) noexcept : value{value} {}
  std::uint64_t value;
  friend auto operator<=>(LogTerm const&, LogTerm const&) = default;
  friend auto operator<<(std::ostream&, LogTerm) -> std::ostream&;

  [[nodiscard]] explicit operator velocypack::Value() const noexcept;
  auto succ() const noexcept -> LogTerm;
};

auto operator<<(std::ostream&, LogTerm) -> std::ostream&;

template<class Inspector>
auto inspect(Inspector& f, LogTerm& x) {
  if constexpr (Inspector::isLoading) {
    auto v = uint64_t{0};
    auto res = f.apply(v);
    if (res.ok()) {
      x = LogTerm(v);
    }
    return res;
  } else {
    return f.apply(x.value);
  }
}

[[nodiscard]] auto to_string(LogTerm term) -> std::string;

}  // namespace arangodb::replication2

namespace arangodb {
template<>
struct velocypack::Extractor<replication2::LogTerm> {
  static auto extract(velocypack::Slice slice) -> replication2::LogTerm {
    return replication2::LogTerm{slice.getNumericValue<std::uint64_t>()};
  }
};
}  // namespace arangodb

template<>
struct std::hash<arangodb::replication2::LogTerm> {
  [[nodiscard]] auto operator()(
      arangodb::replication2::LogTerm const& v) const noexcept -> std::size_t {
    return std::hash<uint64_t>{}(v.value);
  }
};
