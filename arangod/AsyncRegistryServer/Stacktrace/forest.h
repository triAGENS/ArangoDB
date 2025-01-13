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

#include <optional>
#include <vector>
#include <unordered_map>

namespace arangodb::async_registry {

using Id = void*;

template<typename Data>
struct IndexedForest;

/**
   Data structure for a basic forest with nodes of type Node and edges with no
   data.
 */
template<typename Node>
struct Forest {
  auto insert(Id id, Id waiter, Node node) {
    size_t position = _parent.size();
    auto [_iter, was_inserted] = _position.emplace(id, position);
    if (was_inserted) {
      _parent.push_back(waiter);
      _node.push_back(node);
    }
  }
  auto node(Id id) const -> std::optional<Node> {
    auto position = _position.find(id);
    if (position == _position.end()) {
      return std::nullopt;
    }
    return _node[position->second];
  }
  auto index_by_awaitee() -> IndexedForest<Node> {
    std::vector<std::vector<Id>> children{_parent.size(), std::vector<Id>{}};
    for (auto const& [id, position] : _position) {
      auto parent_position = _position.find(_parent[position]);
      if (parent_position != _position.end()) {
        children[parent_position->second].push_back(id);
      }
    }
    return IndexedForest<Node>{
        {std::move(_parent), std::move(_node), std::move(_position)}, children};
  }

  bool operator==(Forest<Node> const&) const = default;

  std::vector<Id> _parent;  // has one entry for each node
  std::vector<Node> _node;  // has one entry for each node
  std::unordered_map<Id, size_t>
      _position;  // at which position of the vectors _waiter and _data to find
                  // entries for Id
};

/**
   Forest that indexes all outgoing edges of a node.
 */
template<typename Node>
struct IndexedForest : Forest<Node> {
  auto children(Id id) const -> std::vector<Id> {
    auto position = Forest<Node>::_position.find(id);
    if (position == Forest<Node>::_position.end()) {
      return std::vector<Id>{};
    }
    return _children[position->second];
  }
  std::vector<std::vector<Id>> _children;
};

template<typename Node>
struct IndexedForestWithRoots;

/**
   Forest that includes a list of its roots
 */
template<typename Node>
struct ForestWithRoots : Forest<Node> {
  ForestWithRoots(Forest<Node> forest, std::vector<Id> roots)
      : Forest<Node>{std::move(forest)}, _roots{std::move(roots)} {}
  auto index_by_awaitee() -> IndexedForestWithRoots<Node> {
    return IndexedForestWithRoots{Forest<Node>::index_by_awaitee(),
                                  std::move(_roots)};
  }
  std::vector<Id> _roots;
};

/**
   Forest with outgoing edge index that includes a list of its roots
 */
template<typename Node>
struct IndexedForestWithRoots : IndexedForest<Node> {
  IndexedForestWithRoots(IndexedForest<Node> forest, std::vector<Id> roots)
      : IndexedForest<Node>{std::move(forest)}, _roots{std::move(roots)} {}
  std::vector<Id> _roots;
};

}  // namespace arangodb::async_registry
