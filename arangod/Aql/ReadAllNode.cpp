////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////


#include "Aql/ReadAllNode.h"

#include "Aql/ReadAllExecutionBlock.h"

using namespace arangodb::aql;

ReadAllNode::ReadAllNode(ExecutionPlan* plan, ExecutionNodeId id)
    : ExecutionNode(plan, id) {}

ReadAllNode::ReadAllNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base) {}

ExecutionNode::NodeType ReadAllNode::getType() const { return READALL; }


/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ReadAllNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  return std::make_unique<ReadAllExecutionBlock>(&engine, this,  createRegisterInfos({}, {}));
}

/// @brief toVelocyPack, for ReadAllNode
void ReadAllNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                       std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  // This node has no own information.
  nodes.close();
}

/// @brief the cost of a singleton is 1, it produces one item only
CostEstimate ReadAllNode::estimateCost() const {
  if (ADB_UNLIKELY(_dependencies.empty())) {
    // we should always have dependency as we need input for fetch all
    TRI_ASSERT(false);
    return aql::CostEstimate::empty();
  }
  // We just forward whatever costs we have above, this block
  // just buffers, and does not apply operations.
  return _dependencies[0]->getCost();
}

