////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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

#include "TermIndexMapping.h"

#include "LogCommon.h"
#include "Basics/debugging.h"
#include "Replication2/Storage/IteratorPosition.h"

using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

auto TermIndexMapping::getTermRange(LogTerm t) const noexcept
    -> std::optional<LogRange> {
  if (auto iter = _mapping.find(t); iter != _mapping.end()) {
    return iter->second.range;
  }
  return std::nullopt;
}

auto TermIndexMapping::getFirstIndexOfTerm(LogTerm term) const noexcept
    -> std::optional<LogIndex> {
  if (auto range = getTermRange(term); range.has_value()) {
    return range->from;
  }
  return std::nullopt;
}

auto operator+(LogTerm t, std::uint64_t delta) -> LogTerm {
  return LogTerm{t.value + delta};
}

void TermIndexMapping::insert(LogRange range,
                              storage::IteratorPosition position,
                              LogTerm term) noexcept {
  TRI_ASSERT(range.from == position.index());
  if (_mapping.empty()) {
    _mapping.emplace(term, TermInfo{range, position});
  } else {
    auto [prevTerm, prevInfo] = *_mapping.rbegin();
    ADB_PROD_ASSERT(prevInfo.range.to == range.from);
    ADB_PROD_ASSERT(prevTerm <= term);
    if (prevTerm == term) {
      _mapping.rbegin()->second.range.to = range.to;
    } else {
      _mapping.emplace_hint(_mapping.end(), term, TermInfo{range, position});
    }
  }
}

void TermIndexMapping::removeFront(LogIndex stop) noexcept {
  auto keep = std::find_if(_mapping.begin(), _mapping.end(), [&](auto pair) {
    return pair.second.range.contains(stop);
  });

  if (keep != _mapping.end()) {
    keep->second.range.from = stop;
  }

  _mapping.erase(_mapping.begin(), keep);
}

void TermIndexMapping::removeBack(LogIndex start) noexcept {
  auto keep = std::find_if(_mapping.rbegin(), _mapping.rend(), [&](auto pair) {
    return pair.second.range.from < start;
  });

  ADB_PROD_ASSERT(_mapping.rbegin().base() == _mapping.end());

  if (keep != _mapping.rend()) {
    keep->second.range.to = start;
  }

  _mapping.erase(keep.base(), _mapping.end());
}

auto TermIndexMapping::getTermOfIndex(LogIndex idx) const noexcept
    -> std::optional<LogTerm> {
  auto term = std::find_if(_mapping.begin(), _mapping.end(), [&](auto pair) {
    return pair.second.range.contains(idx);
  });

  if (term != _mapping.end()) {
    return term->first;
  }
  return std::nullopt;
}

auto TermIndexMapping::getLastIndex() const noexcept
    -> std::optional<TermIndexPair> {
  if (!_mapping.empty()) {
    auto [term, info] = *_mapping.rbegin();
    return TermIndexPair{term, info.range.to.saturatedDecrement()};
  }
  return std::nullopt;
}

auto TermIndexMapping::getFirstIndex() const noexcept
    -> std::optional<TermIndexPair> {
  if (!_mapping.empty()) {
    auto [term, info] = *_mapping.begin();
    return TermIndexPair{term, info.range.from};
  }
  return std::nullopt;
}

void TermIndexMapping::insert(storage::IteratorPosition position,
                              LogTerm term) noexcept {
  auto idx = position.index();
  auto iter = std::invoke([&] {
    if (_mapping.empty()) {
      return _mapping.emplace(term, TermInfo{LogRange{idx, idx}, position})
          .first;
    } else if (auto iter = _mapping.rbegin(); iter->first != term) {
      ADB_PROD_ASSERT(iter->first < term);
      ADB_PROD_ASSERT(iter->second.range.to == idx);

      return _mapping.emplace(term, TermInfo{LogRange{idx, idx}, position})
          .first;
    } else {
      return std::prev(_mapping.end());
    }
  });

  ADB_PROD_ASSERT(iter->second.range.to == idx)
      << "iter->second.range: " << iter->second.range << ", startPos "
      << iter->second.startPosition.index() << "; "
      << iter->second.startPosition.fileOffset() << ", position " << idx << "; "
      << position.fileOffset() << " iter->first: " << iter->first << " term "
      << term;
  iter->second.range.to = idx + 1;
}

void TermIndexMapping::append(TermIndexMapping const& other) noexcept {
  // TODO optimize
  for (auto [term, info] : other._mapping) {
    insert(info.range, info.startPosition, term);
  }
}

auto TermIndexMapping::getIndexRange() const noexcept -> LogRange {
  if (_mapping.empty()) {
    return LogRange{};
  } else {
    auto from = _mapping.begin()->second.range.from;
    auto to = _mapping.rbegin()->second.range.to;
    return LogRange{from, to};
  }
}

auto TermIndexMapping::empty() const noexcept -> bool {
  return _mapping.empty();
}
