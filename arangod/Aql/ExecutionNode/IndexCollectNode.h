////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2025 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once
#include "CollectionAccessingNode.h"
#include "Aql/Expression.h"
#include "ExecutionNode.h"

namespace arangodb {

namespace aql {

struct IndexCollectGroup {
  std::size_t indexField;
  Variable const* outVariable;
};

struct IndexCollectAggregation {
  // aggregator type
  std::string type;
  // output variable
  Variable const* outVariable;
  // aggregation expression. It must only contain attribute accesses to the old
  // document variable that are covered by the index.
  std::unique_ptr<Expression> expression;
};

using IndexCollectGroups = std::vector<IndexCollectGroup>;
using IndexCollectAggregations = std::vector<IndexCollectAggregation>;

struct IndexCollectNode : ExecutionNode, CollectionAccessingNode {
  friend class ExecutionNode;
  friend class ExecutionBlock;

  IndexCollectNode(ExecutionPlan* plan, ExecutionNodeId id,
                   aql::Collection const* collection,
                   std::shared_ptr<arangodb::Index> index,
                   Variable const* oldIndexVariable, IndexCollectGroups groups,
                   IndexCollectAggregations aggregations,
                   CollectOptions collectOptions);

  IndexCollectNode(ExecutionPlan* plan, arangodb::velocypack::Slice slice);

  void replaceVariables(
      std::unordered_map<VariableId, Variable const*> const&) override;

  void replaceAttributeAccess(ExecutionNode const* self,
                              Variable const* searchVariable,
                              std::span<std::string_view> attribute,
                              Variable const* replaceVariable,
                              size_t index) override;

  void getVariablesUsedHere(VarSet& vars) const override;
  std::vector<Variable const*> getVariablesSetHere() const override;

  NodeType getType() const override { return INDEX_COLLECT; }
  size_t getMemoryUsedBytes() const override { return 0; }
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine) const override;

  ExecutionNode* clone(ExecutionPlan* plan,
                       bool withDependencies) const override;

 protected:
  void doToVelocyPack(velocypack::Builder& builder,
                      unsigned int flags) const override;
  CostEstimate estimateCost() const override;

  std::unique_ptr<ExecutionBlock> createBlockDistinctScan(
      ExecutionEngine& engine) const;
  std::unique_ptr<ExecutionBlock> createBlockAggregationScan(
      ExecutionEngine& engine) const;

 private:
  std::shared_ptr<arangodb::Index> _index;
  IndexCollectGroups _groups;
  IndexCollectAggregations _aggregations;
  Variable const* _oldIndexVariable;
  CollectOptions _collectOptions;
};

}  // namespace aql

}  // namespace arangodb
