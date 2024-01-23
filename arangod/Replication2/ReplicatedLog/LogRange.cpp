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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "LogRange.h"

#include <ostream>
#include <Assertions/Assert.h>
#include <Basics/StringUtils.h>

namespace arangodb::replication2 {

LogRange::LogRange(LogIndex from, LogIndex to) noexcept : from(from), to(to) {
  TRI_ASSERT(from <= to);
}

auto LogRange::empty() const noexcept -> bool { return from == to; }

auto LogRange::count() const noexcept -> std::size_t {
  return to.value - from.value;
}

auto LogRange::contains(LogIndex idx) const noexcept -> bool {
  return from <= idx && idx < to;
}

auto LogRange::contains(LogRange other) const noexcept -> bool {
  return from <= other.from && other.to <= to;
}

auto operator<<(std::ostream& os, LogRange const& r) -> std::ostream& {
  return os << "[" << r.from << ", " << r.to << ")";
}

auto intersect(LogRange a, LogRange b) noexcept -> LogRange {
  auto max_from = std::max(a.from, b.from);
  auto min_to = std::min(a.to, b.to);
  if (max_from > min_to) {
    return {};
  } else {
    return {max_from, min_to};
  }
}

auto to_string(LogRange const& r) -> std::string {
  return basics::StringUtils::concatT("[", r.from, ", ", r.to, ")");
}

auto LogRange::end() const noexcept -> LogRange::Iterator {
  return Iterator{to};
}
auto LogRange::begin() const noexcept -> LogRange::Iterator {
  return Iterator{from};
}

auto operator==(LogRange left, LogRange right) noexcept -> bool {
  // Two ranges compare equal iff either both are empty or _from_ and _to_ agree
  return (left.empty() && right.empty()) ||
         (left.from == right.from && left.to == right.to);
}

auto LogRange::Iterator::operator++() noexcept -> LogRange::Iterator& {
  current = current + 1;
  return *this;
}

auto LogRange::Iterator::operator++(int) noexcept -> LogRange::Iterator {
  auto idx = current;
  current = current + 1;
  return Iterator(idx);
}

auto LogRange::Iterator::operator*() const noexcept -> LogIndex {
  return current;
}
auto LogRange::Iterator::operator->() const noexcept -> LogIndex const* {
  return &current;
}

}  // namespace arangodb::replication2
