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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Aql/NodeFinder.h"
#include "Aql/ExecutionNode.h"
#include "Basics/Memory/MemoryTypes.h"

namespace arangodb {
namespace aql {

/// @brief node finder for one node type
template<>
NodeFinder<ExecutionNode::NodeType, WalkerUniqueness::NonUnique>::NodeFinder(
    ExecutionNode::NodeType const& lookingFor,
    containers::SmallVector<ExecutionNode*, 8>& out, bool enterSubqueries)
    : _out(out), _lookingFor(lookingFor), _enterSubqueries(enterSubqueries) {}

/// @brief node finder for multiple types
template<>
NodeFinder<std::initializer_list<ExecutionNode::NodeType>,
           WalkerUniqueness::NonUnique>::
    NodeFinder(std::initializer_list<ExecutionNode::NodeType> const& lookingFor,
               containers::SmallVector<ExecutionNode*, 8>& out,
               bool enterSubqueries)
    : _out(out), _lookingFor(lookingFor), _enterSubqueries(enterSubqueries) {}

/// @brief before method for one node type
template<>
bool NodeFinder<ExecutionNode::NodeType, WalkerUniqueness::NonUnique>::before(
    ExecutionNode* en) {
  if (en->getType() == _lookingFor) {
    _out.emplace_back(en);
  }

  return false;
}

/// @brief before method for multiple node types
template<>
bool NodeFinder<std::initializer_list<ExecutionNode::NodeType>,
                WalkerUniqueness::NonUnique>::before(ExecutionNode* en) {
  auto const nodeType = en->getType();

  for (auto& type : _lookingFor) {
    if (type == nodeType) {
      _out.emplace_back(en);
      break;
    }
  }
  return false;
}

/// @brief unique node finder for multiple types
template<>
NodeFinder<std::initializer_list<ExecutionNode::NodeType>,
           WalkerUniqueness::Unique>::
    NodeFinder(std::initializer_list<ExecutionNode::NodeType> const& lookingFor,
               containers::SmallVector<ExecutionNode*, 8>& out,
               bool enterSubqueries)
    : _out(out), _lookingFor(lookingFor), _enterSubqueries(enterSubqueries) {}

/// @brief before method for multiple node types
template<>
bool NodeFinder<std::initializer_list<ExecutionNode::NodeType>,
                WalkerUniqueness::Unique>::before(ExecutionNode* en) {
  auto const nodeType = en->getType();

  for (auto& type : _lookingFor) {
    if (type == nodeType) {
      _out.emplace_back(en);
      break;
    }
  }
  return false;
}

/// @brief node finder for one node type
template<typename SmallVectorType>
EndNodeFinder<SmallVectorType>::EndNodeFinder(SmallVectorType& out,
                                              bool enterSubqueries)
    : _out(out), _found({false}), _enterSubqueries(enterSubqueries) {}

/// @brief before method for one node type
template<typename SmallVectorType>
bool EndNodeFinder<SmallVectorType>::before(ExecutionNode* en) {
  TRI_ASSERT(!_found.empty());
  if (!_found.back()) {
    // no node found yet. note that we found one on this level
    _out.emplace_back(en);
    _found[_found.size() - 1] = true;
  }

  // if we don't need to enter subqueries, we can stop after the first node that
  // we found
  return (!_enterSubqueries);
}

}  // namespace aql
}  // namespace arangodb
// TEST
using alloc_t =
    arangodb::pmr::polymorphic_allocator<arangodb::aql::ExecutionNode*>;

template<>
class arangodb::aql::EndNodeFinder<arangodb::containers::SmallVector<
    arangodb::aql::ExecutionNode*, 8, alloc_t>>;

template<>
class arangodb::aql::EndNodeFinder<
    arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8>>;
