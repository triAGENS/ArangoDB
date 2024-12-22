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
/// @author Michael Hackstein
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "ShortestPathNode.h"

#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlockImpl.tpp"
#include "Aql/ExecutionPlan.h"
#include "Aql/OptimizerUtils.h"
#include "Aql/ProfileLevel.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/StaticStrings.h"
#include "Graph/ShortestPathOptions.h"
#include "Indexes/Index.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Iterator.h>

#include <memory>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::aql;
using namespace arangodb::graph;

namespace {
void parseNodeInput(AstNode const* node, std::string& id,
                    Variable const*& variable, char const* part) {
  switch (node->type) {
    case NODE_TYPE_REFERENCE:
      variable = static_cast<Variable*>(node->getData());
      id = "";
      break;
    case NODE_TYPE_VALUE:
      if (node->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_QUERY_PARSE,
            absl::StrCat("invalid ", part,
                         " vertex. Must either be an _id string or an object "
                         "with _id."));
      }
      variable = nullptr;
      id = node->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_PARSE,
          absl::StrCat(
              "invalid ", part,
              " vertex. Must either be an _id string or an object with _id."));
  }
}
static GraphNode::InputVertex prepareVertexInput(ShortestPathNode const* node,
                                                 bool isTarget) {
  using InputVertex = GraphNode::InputVertex;
  if (isTarget) {
    if (node->usesTargetInVariable()) {
      auto it =
          node->getRegisterPlan()->varInfo.find(node->targetInVariable()->id);
      TRI_ASSERT(it != node->getRegisterPlan()->varInfo.end());
      return InputVertex{it->second.registerId};
    } else {
      return InputVertex{node->getTargetVertex()};
    }
  } else {
    if (node->usesStartInVariable()) {
      auto it =
          node->getRegisterPlan()->varInfo.find(node->startInVariable()->id);
      TRI_ASSERT(it != node->getRegisterPlan()->varInfo.end());
      return InputVertex{it->second.registerId};
    } else {
      return InputVertex{node->getStartVertex()};
    }
  }
}
}  // namespace

ShortestPathNode::ShortestPathNode(ExecutionPlan* plan, ExecutionNodeId id,
                                   TRI_vocbase_t* vocbase,
                                   AstNode const* direction,
                                   AstNode const* start, AstNode const* target,
                                   AstNode const* graph,
                                   std::unique_ptr<BaseOptions> options)
    : GraphNode(plan, id, vocbase, direction, graph, std::move(options)),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(nullptr) {
  TRI_ASSERT(start != nullptr);
  TRI_ASSERT(target != nullptr);
  TRI_ASSERT(graph != nullptr);

  auto ast = _plan->getAst();
  // Let us build the conditions on _from and _to. Just in case we need them.
  {
    auto const* access = ast->createNodeAttributeAccess(
        getTemporaryRefNode(), StaticStrings::FromString);
    auto const* cond = ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
    _fromCondition = ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    _fromCondition->addMember(cond);
  }
  TRI_ASSERT(_fromCondition != nullptr);

  {
    auto const* access = ast->createNodeAttributeAccess(
        getTemporaryRefNode(), StaticStrings::ToString);
    auto const* cond = ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
    _toCondition = ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    _toCondition->addMember(cond);
  }
  TRI_ASSERT(_toCondition != nullptr);

  parseNodeInput(start, _startVertexId, _inStartVariable, "start");
  parseNodeInput(target, _targetVertexId, _inTargetVariable, "target");
}

/// @brief Internal constructor to clone the node.
ShortestPathNode::ShortestPathNode(
    ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
    std::vector<Collection*> const& edgeColls,
    std::vector<Collection*> const& vertexColls,
    TRI_edge_direction_e defaultDirection,
    std::vector<TRI_edge_direction_e> const& directions,
    Variable const* inStartVariable, std::string const& startVertexId,
    Variable const* inTargetVariable, std::string const& targetVertexId,
    std::unique_ptr<BaseOptions> options, graph::Graph const* graph,
    Variable const* distributeVariable)
    : GraphNode(plan, id, vocbase, edgeColls, vertexColls, defaultDirection,
                directions, std::move(options), graph),
      _inStartVariable(inStartVariable),
      _startVertexId(startVertexId),
      _inTargetVariable(inTargetVariable),
      _targetVertexId(targetVertexId),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(distributeVariable) {}

ShortestPathNode::~ShortestPathNode() = default;

ShortestPathNode::ShortestPathNode(ExecutionPlan* plan, velocypack::Slice base)
    : GraphNode(plan, base),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(nullptr) {
  // Start Vertex
  if (base.hasKey("startInVariable")) {
    _inStartVariable =
        Variable::varFromVPack(plan->getAst(), base, "startInVariable");
  } else {
    VPackSlice v = base.get("startVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex must be a string");
    }
    _startVertexId = v.copyString();

    if (_startVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex mustn't be empty");
    }
  }

  // Target Vertex
  if (base.hasKey("targetInVariable")) {
    _inTargetVariable =
        Variable::varFromVPack(plan->getAst(), base, "targetInVariable");
  } else {
    VPackSlice v = base.get("targetVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex must be a string");
    }
    _targetVertexId = v.copyString();
    if (_targetVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex mustn't be empty");
    }
  }

  // Filter Condition Parts
  TRI_ASSERT(base.hasKey("fromCondition"));
  // the plan's AST takes ownership of the newly created AstNode, so this is
  // safe cppcheck-suppress *
  _fromCondition = plan->getAst()->createNode(base.get("fromCondition"));

  TRI_ASSERT(base.hasKey("toCondition"));
  // the plan's AST takes ownership of the newly created AstNode, so this is
  // safe cppcheck-suppress *
  _toCondition = plan->getAst()->createNode(base.get("toCondition"));

  if (base.hasKey("distributeVariable")) {
    _distributeVariable =
        Variable::varFromVPack(plan->getAst(), base, "distributeVariable");
  }
}

void ShortestPathNode::setStartInVariable(Variable const* inVariable) {
  _inStartVariable = inVariable;
  _startVertexId = "";
}

void ShortestPathNode::setTargetInVariable(Variable const* inVariable) {
  _inTargetVariable = inVariable;
  _targetVertexId = "";
}

void ShortestPathNode::setDistributeVariable(Variable const* distVariable) {
  // Can only be set once.
  TRI_ASSERT(_distributeVariable == nullptr);
  _distributeVariable = distVariable;
}

void ShortestPathNode::doToVelocyPack(VPackBuilder& nodes,
                                      unsigned flags) const {
  GraphNode::doToVelocyPack(nodes, flags);  // call base class method
  // In variables
  if (usesStartInVariable()) {
    nodes.add(VPackValue("startInVariable"));
    startInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("startVertexId", VPackValue(_startVertexId));
  }

  if (usesTargetInVariable()) {
    nodes.add(VPackValue("targetInVariable"));
    targetInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("targetVertexId", VPackValue(_targetVertexId));
  }

  // Out variables
  if (isVertexOutVariableUsedLater()) {
    nodes.add(VPackValue("vertexOutVariable"));
    vertexOutVariable()->toVelocyPack(nodes);
  }
  if (isEdgeOutVariableUsedLater()) {
    nodes.add(VPackValue("edgeOutVariable"));
    edgeOutVariable()->toVelocyPack(nodes);
  }

  // Filter Conditions
  TRI_ASSERT(_fromCondition != nullptr);
  nodes.add(VPackValue("fromCondition"));
  _fromCondition->toVelocyPack(nodes, flags);

  TRI_ASSERT(_toCondition != nullptr);
  nodes.add(VPackValue("toCondition"));
  _toCondition->toVelocyPack(nodes, flags);

  if (_distributeVariable != nullptr) {
    nodes.add(VPackValue("distributeVariable"));
    _distributeVariable->toVelocyPack(nodes);
  }
}

template<typename ShortestPathEnumeratorType>
std::pair<RegIdSet, typename ShortestPathExecutorInfos<
                        ShortestPathEnumeratorType>::RegisterMapping>
ShortestPathNode::_buildOutputRegisters() const {
  auto outputRegisters = RegIdSet{};
  auto& varInfo = getRegisterPlan()->varInfo;

  typename ShortestPathExecutorInfos<
      ShortestPathEnumeratorType>::RegisterMapping outputRegisterMapping;
  if (isVertexOutVariableUsedLater()) {
    auto it = varInfo.find(vertexOutVariable()->id);
    TRI_ASSERT(it != varInfo.end());
    outputRegisterMapping.try_emplace(
        ShortestPathExecutorInfos<
            ShortestPathEnumeratorType>::OutputName::VERTEX,
        it->second.registerId);
    outputRegisters.emplace(it->second.registerId);
  }
  if (isEdgeOutVariableUsedLater()) {
    auto it = varInfo.find(edgeOutVariable()->id);
    TRI_ASSERT(it != varInfo.end());
    outputRegisterMapping.try_emplace(
        ShortestPathExecutorInfos<ShortestPathEnumeratorType>::OutputName::EDGE,
        it->second.registerId);
    outputRegisters.emplace(it->second.registerId);
  }
  return std::make_pair(outputRegisters, outputRegisterMapping);
};

RegIdSet ShortestPathNode::buildVariableInformation() const {
  auto inputRegisters = RegIdSet();
  auto& varInfo = getRegisterPlan()->varInfo;
  if (usesStartInVariable()) {
    auto it = varInfo.find(startInVariable()->id);
    TRI_ASSERT(it != varInfo.end());
    inputRegisters.emplace(it->second.registerId);
  }
  if (usesTargetInVariable()) {
    auto it = varInfo.find(targetInVariable()->id);
    TRI_ASSERT(it != varInfo.end());
    inputRegisters.emplace(it->second.registerId);
  }
  return inputRegisters;
}

// Provider is expected to be:
// 1. TracedKPathEnumerator<SingleServerProvider<SingleServerProviderStep>>
// 2. or SingleServerProvider<SingleServerProviderStep>
// 3. or ClusterProvider<ClusterProviderStep>

template<typename ShortestPath, typename Provider, typename ProviderOptions>
std::unique_ptr<ExecutionBlock> ShortestPathNode::makeExecutionBlockImpl(
    ShortestPathOptions* opts, ProviderOptions forwardProviderOptions,
    ProviderOptions backwardProviderOptions,
    arangodb::graph::TwoSidedEnumeratorOptions enumeratorOptions,
    PathValidatorOptions validatorOptions,
    typename ShortestPathExecutorInfos<ShortestPath>::RegisterMapping&&
        outputRegisterMapping,
    ExecutionEngine& engine, InputVertex sourceInput, InputVertex targetInput,
    RegisterInfos registerInfos) const {
  auto shortestPathFinder = std::make_unique<ShortestPath>(
      Provider{opts->query(), std::move(forwardProviderOptions),
               opts->query().resourceMonitor()},
      Provider{opts->query(), std::move(backwardProviderOptions),
               opts->query().resourceMonitor()},
      std::move(enumeratorOptions), std::move(validatorOptions),
      opts->query().resourceMonitor());

  auto executorInfos = ShortestPathExecutorInfos<ShortestPath>(
      engine.getQuery(), std::move(shortestPathFinder),
      std::move(outputRegisterMapping), std::move(sourceInput),
      std::move(targetInput));

  return std::make_unique<
      ExecutionBlockImpl<ShortestPathExecutor<ShortestPath>>>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
};

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ShortestPathNode::createBlock(
    ExecutionEngine& engine) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  auto inputRegisters = buildVariableInformation();

  auto opts = static_cast<ShortestPathOptions*>(options());

  auto sourceInput = ::prepareVertexInput(this, false);
  auto targetInput = ::prepareVertexInput(this, true);
  auto checkWeight = [&]<typename ProviderOptionsType>(
                         ProviderOptionsType& forwardProviderOptions,
                         ProviderOptionsType& backwardProviderOptions) -> bool {
    if (opts->useWeight()) {
      double defaultWeight = opts->getDefaultWeight();
      if (opts->getWeightAttribute().empty()) {
        forwardProviderOptions.setWeightEdgeCallback(
            [defaultWeight](double previousWeight, VPackSlice edge) -> double {
              return previousWeight + defaultWeight;
            });
        backwardProviderOptions.setWeightEdgeCallback(
            [defaultWeight](double previousWeight, VPackSlice edge) -> double {
              return previousWeight + defaultWeight;
            });
      } else {
        std::string weightAttribute = opts->getWeightAttribute();
        forwardProviderOptions.setWeightEdgeCallback(
            [weightAttribute = weightAttribute, defaultWeight](
                double previousWeight, VPackSlice edge) -> double {
              auto const weight =
                  arangodb::basics::VelocyPackHelper::getNumericValue<double>(
                      edge, weightAttribute, defaultWeight);
              if (weight < 0.) {
                THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
              }

              return previousWeight + weight;
            });
        backwardProviderOptions.setWeightEdgeCallback(
            [weightAttribute = weightAttribute, defaultWeight](
                double previousWeight, VPackSlice edge) -> double {
              auto const weight =
                  arangodb::basics::VelocyPackHelper::getNumericValue<double>(
                      edge, weightAttribute, defaultWeight);
              if (weight < 0.) {
                THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
              }

              return previousWeight + weight;
            });
      }
      return true;
    }
    return false;
  };

  arangodb::graph::TwoSidedEnumeratorOptions enumeratorOptions{
      0, std::numeric_limits<size_t>::max(),
      arangodb::graph::PathType::Type::ShortestPath};
  PathValidatorOptions validatorOptions(opts->tmpVar(),
                                        opts->getExpressionCtx());

  if (!ServerState::instance()->isCoordinator()) {
    std::pair<std::vector<IndexAccessor>,
              std::unordered_map<uint64_t, std::vector<IndexAccessor>>>
        usedIndexes{};
    usedIndexes.first = buildUsedIndexes();

    std::pair<std::vector<IndexAccessor>,
              std::unordered_map<uint64_t, std::vector<IndexAccessor>>>
        reversedUsedIndexes{};
    reversedUsedIndexes.first = buildReverseUsedIndexes();

    SingleServerBaseProviderOptions forwardProviderOptions(
        opts->tmpVar(), std::move(usedIndexes), opts->getExpressionCtx(), {},
        opts->collectionToShard(), opts->getVertexProjections(),
        opts->getEdgeProjections(), opts->produceVertices(), opts->useCache());

    SingleServerBaseProviderOptions backwardProviderOptions(
        opts->tmpVar(), std::move(reversedUsedIndexes),
        opts->getExpressionCtx(), {}, opts->collectionToShard(),
        opts->getVertexProjections(), opts->getEdgeProjections(),
        opts->produceVertices(), opts->useCache());

    auto usesWeight =
        checkWeight(forwardProviderOptions, backwardProviderOptions);

    using Provider = SingleServerProvider<SingleServerProviderStep>;
    if (opts->query().queryOptions().getTraversalProfileLevel() ==
        TraversalProfileLevel::None) {
      // SingleServer Default
      if (usesWeight) {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<WeightedShortestPath>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<
            WeightedShortestPathEnumeratorAlias<Provider>, Provider,
            SingleServerBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      } else {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<ShortestPath>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<ShortestPathEnumerator<Provider>,
                                      Provider,
                                      SingleServerBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      }
    } else {
      // SingleServer Tracing enabled
      if (usesWeight) {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<WeightedShortestPathTracer>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<
            TracedWeightedShortestPathEnumeratorAlias<Provider>,
            ProviderTracer<Provider>, SingleServerBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      } else {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<ShortestPathTracer>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<TracedShortestPathEnumerator<Provider>,
                                      ProviderTracer<Provider>,
                                      SingleServerBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      }
    }
  } else {
    // Cluster
    using ClusterProvider = ClusterProvider<ClusterProviderStep>;
    auto cache = std::make_shared<RefactoredClusterTraverserCache>(
        opts->query().resourceMonitor());
    ClusterBaseProviderOptions forwardProviderOptions(cache, engines(), false,
                                                      opts->produceVertices());
    forwardProviderOptions.setClearEdgeCacheOnClear(false);
    forwardProviderOptions.setDepthSpecificLookup(false);
    ClusterBaseProviderOptions backwardProviderOptions(cache, engines(), true,
                                                       opts->produceVertices());
    backwardProviderOptions.setClearEdgeCacheOnClear(false);
    backwardProviderOptions.setDepthSpecificLookup(false);

    auto usesWeight =
        checkWeight(forwardProviderOptions, backwardProviderOptions);

    if (opts->query().queryOptions().getTraversalProfileLevel() ==
        TraversalProfileLevel::None) {
      // No tracing
      if (usesWeight) {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<WeightedShortestPathCluster>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<
            WeightedShortestPathEnumeratorAlias<ClusterProvider>,
            ClusterProvider, ClusterBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      } else {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<ShortestPathCluster>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));
        return makeExecutionBlockImpl<ShortestPathEnumerator<ClusterProvider>,
                                      ClusterProvider,
                                      ClusterBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      }
    } else {
      if (usesWeight) {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<WeightedShortestPathClusterTracer>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));

        return makeExecutionBlockImpl<
            TracedWeightedShortestPathEnumeratorAlias<ClusterProvider>,
            ProviderTracer<ClusterProvider>, ClusterBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      } else {
        auto [outputRegisters, outputRegisterMapping] =
            _buildOutputRegisters<ShortestPathClusterTracer>();
        auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                                 std::move(outputRegisters));

        return makeExecutionBlockImpl<
            TracedShortestPathEnumerator<ClusterProvider>,
            ProviderTracer<ClusterProvider>, ClusterBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, std::move(outputRegisterMapping), engine,
            sourceInput, targetInput, std::move(registerInfos));
      }
    }
  }
  TRI_ASSERT(false);
}

ExecutionNode* ShortestPathNode::clone(ExecutionPlan* plan,
                                       bool withDependencies) const {
  TRI_ASSERT(!_optionsBuilt);
  auto oldOpts = static_cast<ShortestPathOptions*>(options());
  std::unique_ptr<BaseOptions> tmp =
      std::make_unique<ShortestPathOptions>(*oldOpts);
  auto c = std::make_unique<ShortestPathNode>(
      plan, _id, _vocbase, _edgeColls, _vertexColls, _defaultDirection,
      _directions, _inStartVariable, _startVertexId, _inTargetVariable,
      _targetVertexId, std::move(tmp), _graphObj, _distributeVariable);
  shortestPathCloneHelper(*plan, *c);

  return cloneHelper(std::move(c), withDependencies);
}

void ShortestPathNode::shortestPathCloneHelper(ExecutionPlan& plan,
                                               ShortestPathNode& c) const {
  graphCloneHelper(plan, c);
  if (isVertexOutVariableUsedLater()) {
    TRI_ASSERT(_vertexOutVariable != nullptr);
    c.setVertexOutput(_vertexOutVariable);
  }

  if (isEdgeOutVariableUsedLater()) {
    TRI_ASSERT(_edgeOutVariable != nullptr);
    c.setEdgeOutput(_edgeOutVariable);
  }

  // Temporary Filter Objects
  c._tmpObjVariable = _tmpObjVariable;
  c._tmpObjVarNode = _tmpObjVarNode;
  c._tmpIdNode = _tmpIdNode;

  // Filter Condition Parts
  c._fromCondition = _fromCondition->clone(_plan->getAst());
  c._toCondition = _toCondition->clone(_plan->getAst());
}

void ShortestPathNode::replaceVariables(
    std::unordered_map<VariableId, Variable const*> const& replacements) {
  if (_inStartVariable != nullptr) {
    _inStartVariable = Variable::replace(_inStartVariable, replacements);
  }

  if (_inTargetVariable != nullptr) {
    _inTargetVariable = Variable::replace(_inTargetVariable, replacements);
  }

  if (_distributeVariable != nullptr) {
    _distributeVariable = Variable::replace(_distributeVariable, replacements);
  }

  if (_fromCondition != nullptr) {
    _fromCondition = Ast::replaceVariables(_fromCondition, replacements, true);
  }

  if (_toCondition != nullptr) {
    _toCondition = Ast::replaceVariables(_toCondition, replacements, true);
  }
}

void ShortestPathNode::replaceAttributeAccess(
    ExecutionNode const* self, Variable const* searchVariable,
    std::span<std::string_view> attribute, Variable const* replaceVariable,
    size_t /*index*/) {
  if (_inStartVariable != nullptr && searchVariable == _inStartVariable &&
      attribute.size() == 1 && attribute[0] == StaticStrings::IdString) {
    _inStartVariable = replaceVariable;
  }

  if (_inTargetVariable != nullptr && searchVariable == _inTargetVariable &&
      attribute.size() == 1 && attribute[0] == StaticStrings::IdString) {
    _inTargetVariable = replaceVariable;
  }
  // note: _distributeVariable does not need to be replaced, as it is only
  // populated by the optimizer, using a temporary calculation that the
  // optimizer just inserted and that invokes any of the MAKE_DISTRIBUTE_...
  // internal functions.

  if (_fromCondition != nullptr) {
    _fromCondition =
        Ast::replaceAttributeAccess(_plan->getAst(), _fromCondition,
                                    searchVariable, attribute, replaceVariable);
  }

  if (_toCondition != nullptr) {
    _toCondition =
        Ast::replaceAttributeAccess(_plan->getAst(), _toCondition,
                                    searchVariable, attribute, replaceVariable);
  }
}

/// @brief getVariablesSetHere
std::vector<Variable const*> ShortestPathNode::getVariablesSetHere() const {
  std::vector<Variable const*> vars;
  if (isVertexOutVariableUsedLater()) {
    vars.emplace_back(vertexOutVariable());
  }
  if (isEdgeOutVariableUsedLater()) {
    vars.emplace_back(edgeOutVariable());
  }
  return vars;
}

/// @brief getVariablesUsedHere, modifying the set in-place
void ShortestPathNode::getVariablesUsedHere(VarSet& vars) const {
  if (_inStartVariable != nullptr) {
    vars.emplace(_inStartVariable);
  }
  if (_inTargetVariable != nullptr) {
    vars.emplace(_inTargetVariable);
  }
  if (_distributeVariable != nullptr) {
    // NOTE: This is not actually used, but is required to be kept
    // as the shard based distribute node is eventually inserted, after
    // the register plan is completed, and requires this variable.
    // if we do not retain it here, the shard based distribution would
    // not be able to distribute request to shard.
    vars.emplace(_distributeVariable);
  }
}

std::vector<arangodb::graph::IndexAccessor> ShortestPathNode::buildUsedIndexes()
    const {
  // TODO [GraphRefactor]: Re-used functionality here. Move this method to a
  // dedicated place where it can be re-used.
  return buildIndexes(/*reverse*/ false);
}

std::vector<arangodb::graph::IndexAccessor>
ShortestPathNode::buildReverseUsedIndexes() const {
  // TODO [GraphRefactor]: Re-used functionality here. Move this method to a
  // dedicated place where it can be re-used.
  return buildIndexes(/*reverse*/ true);
}

std::vector<arangodb::graph::IndexAccessor> ShortestPathNode::buildIndexes(
    bool reverse) const {
  // TODO [GraphRefactor]: Re-used functionality here (Origin:
  // EnumeratePathsNode.cpp). Move this method to a dedicated place where it can
  // be re-used.
  size_t numEdgeColls = _edgeColls.size();

  std::vector<IndexAccessor> indexAccessors;
  indexAccessors.reserve(numEdgeColls);

  for (size_t i = 0; i < numEdgeColls; ++i) {
    auto dir = _directions[i];
    TRI_ASSERT(dir == TRI_EDGE_IN || dir == TRI_EDGE_OUT);
    auto opposite = (dir == TRI_EDGE_IN ? TRI_EDGE_OUT : TRI_EDGE_IN);

    std::shared_ptr<Index> indexToUse;
    aql::AstNode* condition =
        ((dir == TRI_EDGE_IN) != reverse) ? _toCondition : _fromCondition;
    aql::AstNode* clonedCondition = condition->clone(_plan->getAst());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // very expensive, but only used for assertions:
    // we are stringifying the contents of clonedCondition to figure out
    // if the call to getBestIndexHandleForFilterCondition modifies it.
    std::string const conditionString = clonedCondition->toString();
#endif
    // arbitrary value for "number of edges in collection" used here. the
    // actual value does not matter much. 1000 has historically worked fine.
    constexpr size_t itemsInCollection = 1000;

    // use most specific index hint here.
    // TODO: this code is prepared to use index hints, but due to the
    // "onlyEdgeIndexes" flag set to true here, the optimizer will _always_ pick
    // the edge index for the shortest path query. we should fix the condition
    // handling inside shortest path queries so that it can work with arbitrary,
    // multi-field conditions and thus indexes.
    auto indexHint = hint().getFromNested(
        (reverse ? opposite : dir) == TRI_EDGE_IN ? "inbound" : "outbound",
        _edgeColls[i]->name(), IndexHint::BaseDepth);

    auto& trx = plan()->getAst()->query().trxForOptimization();
    bool res = aql::utils::getBestIndexHandleForFilterCondition(
        trx, *_edgeColls[i], clonedCondition, options()->tmpVar(),
        itemsInCollection, indexHint, indexToUse, ReadOwnWrites::no,
        /*onlyEdgeIndexes*/ true);
    if (!res) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "expected edge index not found");
    }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // check that getBestIndexHandleForFilterCondition did not modify
    // clonedCondition in any way.
    // this assumption must hold true, because we can only use an edge
    // index in this method (onlyEdgeIndexes == true) and edge conditions
    // are always simple.
    TRI_ASSERT(clonedCondition->toString() == conditionString);
#endif

    indexAccessors.emplace_back(std::move(indexToUse), clonedCondition, 0,
                                nullptr, std::nullopt, i,
                                reverse ? opposite : dir);
  }

  return indexAccessors;
}

void ShortestPathNode::prepareOptions() {
  if (_optionsBuilt) {
    return;
  }
  TRI_ASSERT(!_optionsBuilt);

  size_t numEdgeColls = _edgeColls.size();
  Ast* ast = _plan->getAst();
  auto opts = static_cast<ShortestPathOptions*>(options());

  opts->setVariable(getTemporaryVariable());

  // Compute Indexes.
  for (size_t i = 0; i < numEdgeColls; ++i) {
    auto dir = _directions[i];
    switch (dir) {
      case TRI_EDGE_IN:
        opts->addLookupInfo(_plan, _edgeColls[i]->name(),
                            StaticStrings::ToString, _toCondition->clone(ast),
                            /*onlyEdgeindexes*/ false, dir);
        opts->addReverseLookupInfo(_plan, _edgeColls[i]->name(),
                                   StaticStrings::FromString,
                                   _fromCondition->clone(ast),
                                   /*onlyEdgeIndexes*/ false, TRI_EDGE_OUT);
        break;
      case TRI_EDGE_OUT:
        opts->addLookupInfo(
            _plan, _edgeColls[i]->name(), StaticStrings::FromString,
            _fromCondition->clone(ast), /*onlyEdgeIndexes*/ false, dir);
        opts->addReverseLookupInfo(
            _plan, _edgeColls[i]->name(), StaticStrings::ToString,
            _toCondition->clone(ast), /*onlyEdgeIndexes*/ false, TRI_EDGE_IN);
        break;
      case TRI_EDGE_ANY:
        TRI_ASSERT(false);
        break;
    }
  }
  // If we use the path output the cache should activate document
  // caching otherwise it is not worth it.
  if (ServerState::instance()->isCoordinator()) {
    _options->activateCache(false, engines());
  } else {
    _options->activateCache(false, nullptr);
  }
  _optionsBuilt = true;
}

auto ShortestPathNode::options() const -> ShortestPathOptions* {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* opts = dynamic_cast<ShortestPathOptions*>(GraphNode::options());
  TRI_ASSERT((GraphNode::options() == nullptr) == (opts == nullptr));
#else
  auto* opts = static_cast<ShortestPathOptions*>(GraphNode::options());
#endif
  return opts;
}

size_t ShortestPathNode::getMemoryUsedBytes() const { return sizeof(*this); }

// This constructor is only used from LocalTraversalNode, and GraphNode
// is virtually inherited; thus its constructor is never called from here.
ShortestPathNode::ShortestPathNode(ExecutionPlan& plan,
                                   ShortestPathNode const& other)
    : GraphNode(GraphNode::THIS_THROWS_WHEN_CALLED{}),
      _inStartVariable(other._inStartVariable),
      _startVertexId(other._startVertexId),
      _inTargetVariable(other._inTargetVariable),
      _targetVertexId(other._targetVertexId),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(other._distributeVariable) {
  other.shortestPathCloneHelper(plan, *this);
}
