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
/// @author Michael Hackstein
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/GraphNode.h"
#include "Aql/Graphs.h"
#include "Graph/PathType.h"
#include "Graph/Providers/BaseProviderOptions.h"
#include "Graph/Options/TwoSidedEnumeratorOptions.h"
#include "Graph/PathManagement/PathValidatorOptions.h"

namespace arangodb {

namespace velocypack {
class Builder;
}

namespace graph {
struct BaseOptions;
struct ShortestPathOptions;
struct IndexAccessor;
}  // namespace graph
namespace aql {

class EnumeratePathsNode : public virtual GraphNode {
  friend class ExecutionBlock;

  /// @brief constructor with a vocbase and a collection name
 protected:
  /// @brief Clone constructor, used for constructors of derived classes.
  /// Does not clone recursively, does not clone properties (`other.plan()` is
  /// expected to be the same as `plan)`, and does not register this node in the
  /// plan.
  EnumeratePathsNode(ExecutionPlan& plan, EnumeratePathsNode const& node);

 public:
  EnumeratePathsNode(ExecutionPlan* plan, ExecutionNodeId id,
                     TRI_vocbase_t* vocbase,
                     arangodb::graph::PathType::Type pathType,
                     AstNode const* direction, AstNode const* start,
                     AstNode const* target, AstNode const* graph,
                     std::unique_ptr<graph::BaseOptions> options);

  EnumeratePathsNode(ExecutionPlan* plan,
                     arangodb::velocypack::Slice const& base);

  ~EnumeratePathsNode();

  /// @brief Internal constructor to clone the node.
  EnumeratePathsNode(
      ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
      arangodb::graph::PathType::Type pathType,
      std::vector<Collection*> const& edgeColls,
      std::vector<Collection*> const& vertexColls,
      TRI_edge_direction_e defaultDirection,
      std::vector<TRI_edge_direction_e> const& directions,
      Variable const* inStartVariable, std::string const& startVertexId,
      Variable const* inTargetVariable, std::string const& targetVertexId,
      std::unique_ptr<graph::BaseOptions> options, graph::Graph const* graph,
      Variable const* distributeVariable);

 public:
  /// @brief return the type of the node
  NodeType getType() const override final { return ENUMERATE_PATHS; }

  /// @brief return the amount of bytes used
  size_t getMemoryUsedBytes() const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override;

  bool usesPathOutVariable() const { return _pathOutVariable != nullptr; }
  Variable const& pathOutVariable() const {
    TRI_ASSERT(_pathOutVariable != nullptr);
    return *_pathOutVariable;
  }

  void setPathOutput(Variable const* outVar) { _pathOutVariable = outVar; }

  /// @brief Test if this node uses an in variable or constant for start
  bool usesStartInVariable() const { return _inStartVariable != nullptr; }

  /// @brief return the start variable
  Variable const& startInVariable() const {
    TRI_ASSERT(_inStartVariable != nullptr);
    return *_inStartVariable;
  }

  void setStartInVariable(Variable const* inVariable);

  void setTargetInVariable(Variable const* inVariable);
  void setDistributeVariable(Variable const* distVariable);

  std::string const getStartVertex() const { return _startVertexId; }

  /// @brief Test if this node uses an in variable or constant for target
  bool usesTargetInVariable() const { return _inTargetVariable != nullptr; }

  /// @brief return the target variable
  Variable const& targetInVariable() const {
    TRI_ASSERT(_inTargetVariable != nullptr);
    return *_inTargetVariable;
  }

  std::string const getTargetVertex() const { return _targetVertexId; }

  void replaceVariables(std::unordered_map<VariableId, Variable const*> const&
                            replacements) override;

  /// @brief getVariablesSetHere
  std::vector<Variable const*> getVariablesSetHere() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override;

  /// @brief algorithm type (K_SHORTEST_PATHS or K_PATHS)
  arangodb::graph::PathType::Type pathType() const { return _pathType; }

  /// @brief Compute the shortest path options containing the expressions
  ///        MUST! be called after optimization and before creation
  ///        of blocks.
  void prepareOptions() override;

  std::vector<arangodb::graph::IndexAccessor> buildUsedIndexes() const;

  std::vector<arangodb::graph::IndexAccessor> buildReverseUsedIndexes() const;

  /// @brief Overrides GraphNode::options() with a more specific return type
  ///  (casts graph::BaseOptions* into graph::ShortestPathOptions*)
  auto options() const -> graph::ShortestPathOptions*;

  auto registerGlobalVertexCondition(AstNode const*) -> void;
  auto registerGlobalEdgeCondition(AstNode const*) -> void;

 protected:
  /// @brief export to VelocyPack
  void doToVelocyPack(arangodb::velocypack::Builder&,
                      unsigned flags) const override final;

 private:
  std::vector<arangodb::graph::IndexAccessor> buildIndexes(bool reverse) const;

  void enumeratePathsCloneHelper(ExecutionPlan& plan, EnumeratePathsNode& c,
                                 bool withProperties) const;

  /// @brief algorithm type (K_SHORTEST_PATHS or K_PATHS)
  arangodb::graph::PathType::Type _pathType;

  /// @brief path output variable
  Variable const* _pathOutVariable;

  /// @brief input variable only used if _vertexId is unused
  Variable const* _inStartVariable;

  /// @brief input vertexId only used if _inVariable is unused
  std::string _startVertexId;

  /// @brief input variable only used if _vertexId is unused
  Variable const* _inTargetVariable;

  /// @brief input vertexId only used if _inVariable is unused
  std::string _targetVertexId;

  /// @brief The hard coded condition on _from
  AstNode* _fromCondition;

  /// @brief The hard coded condition on _to
  AstNode* _toCondition;

  /// @brief Global conditions on vertices; if these conditions
  /// are not satisfied the path search is stopped from there.
  std::vector<AstNode const*> _globalVertexConditions;

  /// @brief Global conditions on edges; if this conditions are not
  // satisfied the path search is stopped from here
  std::vector<AstNode const*> _globalEdgeConditions;

  /// @brief Variable that contains the value on which the distribution is
  /// determined. This is required for hybrid disjoint smart graphs
  /// Note: The variable is not really used here, but we need to make sure
  /// it is retained up to this point node for the query instantiation,
  /// otherwise it will be discarded on coordinator (mapped to server), but the
  /// DBServer cannot map the Vertex to its shard.
  Variable const* _distributeVariable;

  template<typename KPath, typename Provider, typename ProviderOptions>
  std::unique_ptr<ExecutionBlock> _makeExecutionBlockImpl(
      graph::ShortestPathOptions* opts, ProviderOptions forwardProviderOptions,
      ProviderOptions backwardProviderOptions,
      arangodb::graph::TwoSidedEnumeratorOptions enumeratorOptions,
      arangodb::graph::PathValidatorOptions validatorOptions,
      const RegisterId& outputRegister, ExecutionEngine& engine,
      InputVertex sourceInput, InputVertex targetInput,
      RegisterInfos registerInfos) const;
};

}  // namespace aql
}  // namespace arangodb
