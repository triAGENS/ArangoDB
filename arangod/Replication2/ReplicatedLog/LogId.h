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

#include <optional>
#include <string_view>
#include <Basics/Identifier.h>
#include <velocypack/Slice.h>

namespace arangodb::replication2 {

class LogId : public arangodb::basics::Identifier {
 public:
  using arangodb::basics::Identifier::Identifier;

  static auto fromString(std::string_view) noexcept -> std::optional<LogId>;

  [[nodiscard]] explicit operator velocypack::Value() const noexcept;
};

template<class Inspector>
auto inspect(Inspector& f, LogId& x) {
  if constexpr (Inspector::isLoading) {
    auto v = uint64_t{0};
    auto res = f.apply(v);
    if (res.ok()) {
      x = LogId(v);
    }
    return res;
  } else {
    // TODO this is a hack to make the compiler happy who does not want
    //      to assign x.id() (unsigned long int) to what it expects (unsigned
    //      long int&)
    auto id = x.id();
    return f.apply(id);
  }
}

auto to_string(LogId logId) -> std::string;

}  // namespace arangodb::replication2

namespace arangodb {
template<>
struct velocypack::Extractor<replication2::LogId> {
  static auto extract(velocypack::Slice slice) -> replication2::LogId {
    return replication2::LogId{slice.getNumericValue<std::uint64_t>()};
  }
};

}  // namespace arangodb

template<>
struct fmt::formatter<arangodb::replication2::LogId>
    : fmt::formatter<arangodb::basics::Identifier> {};

template<>
struct std::hash<arangodb::replication2::LogId> {
  [[nodiscard]] auto operator()(
      arangodb::replication2::LogId const& v) const noexcept -> std::size_t {
    return std::hash<arangodb::basics::Identifier>{}(v);
  }
};
