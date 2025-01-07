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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <optional>
#include <stack>
#include <vector>
#include <unordered_map>

namespace arangodb::async_registry {

using Id = void*;

template<typename Data>
struct IndexedForest;

template<typename Data>
struct WaiterForest {
  auto insert(Id id, Id waiter, Data data) {
    size_t position = _waiter.size();
    auto [_iter, was_inserted] = _position.emplace(id, position);
    if (was_inserted) {
      _waiter.push_back(waiter);
      _data.push_back(data);
    }
  }
  auto data(Id id) const -> std::optional<Data> {
    auto position = _position.find(id);
    if (position == _position.end()) {
      return std::nullopt;
    }
    return _data[position->second];
  }
  auto index_by_awaitee() -> IndexedForest<Data> {
    std::vector<std::vector<Id>> children{_waiter.size(), std::vector<Id>{}};
    for (auto const& [id, position] : _position) {
      auto waiter_position = _position.find(_waiter[position]);
      if (waiter_position != _position.end()) {
        children[waiter_position->second].push_back(id);
      }
    }
    return IndexedForest<Data>{
        {std::move(_position), std::move(_waiter), std::move(_data)}, children};
  }

  bool operator==(WaiterForest<Data> const&) const = default;

  std::unordered_map<Id, size_t>
      _position;  // at which position of the vectors _waiter and _data to find
                  // entries for Id
  std::vector<Id> _waiter;
  std::vector<Data> _data;
};
template<typename Data>
struct IndexedForest : WaiterForest<Data> {
  auto children(Id id) const -> std::vector<Id> {
    auto position = WaiterForest<Data>::_position.find(id);
    if (position == WaiterForest<Data>::_position.end()) {
      return std::vector<Id>{};
    }
    return _children[position->second];
  }
  std::vector<std::vector<Id>> _children;
};

template<typename Data>
struct IndexedForestWithRoots;

template<typename Data>
struct ForestWithRoots : WaiterForest<Data> {
  ForestWithRoots(WaiterForest<Data> forest, std::vector<Id> roots)
      : WaiterForest<Data>{std::move(forest)}, _roots{std::move(roots)} {}
  auto index_by_awaitee() -> IndexedForestWithRoots<Data> {
    return IndexedForestWithRoots{WaiterForest<Data>::index_by_awaitee(),
                                  std::move(_roots)};
  }
  std::vector<Id> _roots;
};
template<typename Data>
struct IndexedForestWithRoots : IndexedForest<Data> {
  IndexedForestWithRoots(IndexedForest<Data> forest, std::vector<Id> roots)
      : IndexedForest<Data>{std::move(forest)}, _roots{std::move(roots)} {}
  std::vector<Id> _roots;
};

using TreeHierarchy = size_t;

template<typename T>
concept HasChildren = requires(T t, Id id) {
  { t.children(id) } -> std::convertible_to<std::vector<Id>>;
};

template<HasChildren Forest>
struct DFS_PostOrder {
  DFS_PostOrder(Forest& forest, Id start) : _forest{forest}, _start{start} {
    _stack.push(std::make_tuple(start, 0, false));
  }
  auto next() -> std::optional<std::pair<Id, TreeHierarchy>> {
    if (_stack.empty()) {
      return std::nullopt;
    }
    auto [item, hierarchy, hasChildrenProcessed] = _stack.top();
    _stack.pop();
    if (hasChildrenProcessed) {
      return std::make_pair(item, hierarchy);
    } else {
      auto children = _forest.children(item);
      if (children.empty()) {
        return std::make_pair(item, hierarchy);
      } else {
        _stack.push(std::make_tuple(item, hierarchy, true));
        for (auto const& child : children) {
          _stack.push(std::make_tuple(child, hierarchy + 1, false));
        }
      }
    }
    return next();
  }

  Forest& _forest;
  Id _start;
  std::stack<std::tuple<Id, TreeHierarchy, bool>> _stack;
};

}  // namespace arangodb::async_registry
