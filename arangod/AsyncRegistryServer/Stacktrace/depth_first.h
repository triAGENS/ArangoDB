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

namespace arangodb::async_registry {

using Id = void*;
using TreeHierarchy = size_t;

template<typename T>
concept HasChildren = requires(T t, Id id) {
  { t.children(id) } -> std::convertible_to<std::vector<Id>>;
};

/**
   Iterator for traversing an outgoing edge indexed forest depth first in post
   order, starting from node start.
 */
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
