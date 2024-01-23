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

#include <iosfwd>
#include "Replication2/ReplicatedLog/LogIndex.h"

namespace arangodb::replication2 {

struct LogRange {
  LogIndex from{0};
  LogIndex to{0};

  LogRange() noexcept = default;
  LogRange(LogIndex from, LogIndex to) noexcept;

  [[nodiscard]] auto empty() const noexcept -> bool;
  [[nodiscard]] auto count() const noexcept -> std::size_t;
  [[nodiscard]] auto contains(LogIndex idx) const noexcept -> bool;
  [[nodiscard]] auto contains(LogRange idx) const noexcept -> bool;

  friend auto operator<<(std::ostream& os, LogRange const& r) -> std::ostream&;
  friend auto intersect(LogRange a, LogRange b) noexcept -> LogRange;

  struct Iterator {
    auto operator++() noexcept -> Iterator&;
    auto operator++(int) noexcept -> Iterator;
    auto operator*() const noexcept -> LogIndex;
    auto operator->() const noexcept -> LogIndex const*;
    friend auto operator==(Iterator const& a, Iterator const& b) noexcept
        -> bool = default;

   private:
    friend LogRange;
    explicit Iterator(LogIndex idx) : current(idx) {}
    LogIndex current;
  };

  [[nodiscard]] auto begin() const noexcept -> Iterator;
  [[nodiscard]] auto end() const noexcept -> Iterator;
};

auto operator==(LogRange, LogRange) noexcept -> bool;
auto operator<<(std::ostream& os, LogRange const& r) -> std::ostream&;

template<class Inspector>
auto inspect(Inspector& f, LogRange& x) {
  return f.object(x).fields(f.field("from", x.from), f.field("to", x.to));
}

auto intersect(LogRange a, LogRange b) noexcept -> LogRange;
auto to_string(LogRange const&) -> std::string;

}  // namespace arangodb::replication2
