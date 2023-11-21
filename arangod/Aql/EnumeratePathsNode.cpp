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
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "EnumeratePathsNode.h"

#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlockImpl.tpp"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/EnumeratePathsExecutor.h"
#include "Aql/ProfileLevel.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Graph/Enumerators/TwoSidedEnumerator.h"
#include "Graph/PathManagement/PathStore.h"
#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/ProviderTracer.h"
#include "Graph/Providers/SingleServerProvider.h"
#include "Graph/Queues/FifoQueue.h"
#include "Graph/ShortestPathOptions.h"
#include "Indexes/Index.h"
#include "OptimizerUtils.h"
#include "Utils/CollectionNameResolver.h"

#include "Graph/algorithm-aliases.h"

#include <velocypack/Iterator.h>

#include <memory>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::aql;
using namespace arangodb::graph;

namespace {
static void parseNodeInput(AstNode const* node, std::string& id,
                           Variable const*& variable, char const* part) {
  switch (node->type) {
    case NODE_TYPE_REFERENCE:
      variable = static_cast<Variable*>(node->getData());
      id = "";
      break;
    case NODE_TYPE_VALUE:
      if (node->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_QUERY_PARSE, std::string("invalid ") + part +
                                       " vertex. Must either be "
                                       "an _id string or an object with _id.");
      }
      variable = nullptr;
      id = node->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     std::string("invalid ") + part +
                                         " vertex. Must either be an "
                                         "_id string or an object with _id.");
  }
}

static GraphNode::InputVertex prepareVertexInput(EnumeratePathsNode const* node,
                                                 bool isTarget) {
  using InputVertex = GraphNode::InputVertex;
  if (isTarget) {
    if (node->usesTargetInVariable()) {
      auto it =
          node->getRegisterPlan()->varInfo.find(node->targetInVariable().id);
      TRI_ASSERT(it != node->getRegisterPlan()->varInfo.end());
      return InputVertex{it->second.registerId};
    } else {
      return InputVertex{node->getTargetVertex()};
    }
  } else {
    if (node->usesStartInVariable()) {
      auto it =
          node->getRegisterPlan()->varInfo.find(node->startInVariable().id);
      TRI_ASSERT(it != node->getRegisterPlan()->varInfo.end());
      return InputVertex{it->second.registerId};
    } else {
      return InputVertex{node->getStartVertex()};
    }
  }
}
}  // namespace

EnumeratePathsNode::EnumeratePathsNode(
    ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
    arangodb::graph::PathType::Type pathType, AstNode const* direction,
    AstNode const* start, AstNode const* target, AstNode const* graph,
    std::unique_ptr<BaseOptions> options)
    : GraphNode(plan, id, vocbase, direction, graph, std::move(options)),
      _pathType(pathType),
      _pathOutVariable(nullptr),
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
EnumeratePathsNode::EnumeratePathsNode(
    ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
    arangodb::graph::PathType::Type pathType,
    std::vector<Collection*> const& edgeColls,
    std::vector<Collection*> const& vertexColls,
    TRI_edge_direction_e defaultDirection,
    std::vector<TRI_edge_direction_e> const& directions,
    Variable const* inStartVariable, std::string const& startVertexId,
    Variable const* inTargetVariable, std::string const& targetVertexId,
    std::unique_ptr<graph::BaseOptions> options, graph::Graph const* graph,
    Variable const* distributeVariable)
    : GraphNode(plan, id, vocbase, edgeColls, vertexColls, defaultDirection,
                directions, std::move(options), graph),
      _pathType(pathType),
      _pathOutVariable(nullptr),
      _inStartVariable(inStartVariable),
      _startVertexId(startVertexId),
      _inTargetVariable(inTargetVariable),
      _targetVertexId(targetVertexId),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(distributeVariable) {}

EnumeratePathsNode::~EnumeratePathsNode() = default;

EnumeratePathsNode::EnumeratePathsNode(ExecutionPlan* plan,
                                       arangodb::velocypack::Slice const& base)
    : GraphNode(plan, base),
      _pathType(arangodb::graph::PathType::Type::KShortestPaths),
      _pathOutVariable(nullptr),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _globalVertexConditions(),
      _globalEdgeConditions(),
      _distributeVariable(nullptr) {
  if (base.hasKey(StaticStrings::GraphQueryShortestPathType)) {
    _pathType = arangodb::graph::PathType::fromString(
        base.get(StaticStrings::GraphQueryShortestPathType)
            .copyString()
            .c_str());
  }

  // Path out variable
  if (base.hasKey("pathOutVariable")) {
    _pathOutVariable =
        Variable::varFromVPack(plan->getAst(), base, "pathOutVariable");
  }

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

  {
    auto list = base.get("globalEdgeConditions");
    if (list.isArray()) {
      for (auto const& cond : VPackArrayIterator(list)) {
        _globalEdgeConditions.emplace_back(plan->getAst()->createNode(cond));
      }
    }
  }

  {
    auto list = base.get("globalVertexConditions");
    if (list.isArray()) {
      for (auto const& cond : VPackArrayIterator(list)) {
        _globalVertexConditions.emplace_back(plan->getAst()->createNode(cond));
      }
    }
  }

  if (base.hasKey("distributeVariable")) {
    _distributeVariable =
        Variable::varFromVPack(plan->getAst(), base, "distributeVariable");
  }
}

// This constructor is only used from LocalTraversalNode, and GraphNode
// is virtually inherited; thus its constructor is never called from here.
EnumeratePathsNode::EnumeratePathsNode(ExecutionPlan& plan,
                                       EnumeratePathsNode const& other)
    : GraphNode(GraphNode::THIS_THROWS_WHEN_CALLED{}),
      _pathType(other._pathType),
      _pathOutVariable(other._pathOutVariable),
      _inStartVariable(other._inStartVariable),
      _startVertexId(other._startVertexId),
      _inTargetVariable(other._inTargetVariable),
      _targetVertexId(other._targetVertexId),
      _fromCondition(nullptr),
      _toCondition(nullptr),
      _distributeVariable(nullptr) {
  other.enumeratePathsCloneHelper(plan, *this, false);
}

void EnumeratePathsNode::setStartInVariable(Variable const* inVariable) {
  _inStartVariable = inVariable;
  _startVertexId = "";
}

void EnumeratePathsNode::setTargetInVariable(Variable const* inVariable) {
  _inTargetVariable = inVariable;
  _targetVertexId = "";
}

void EnumeratePathsNode::setDistributeVariable(Variable const* distVariable) {
  // Can only be set once.
  TRI_ASSERT(_distributeVariable == nullptr);
  _distributeVariable = distVariable;
}

void EnumeratePathsNode::doToVelocyPack(VPackBuilder& nodes,
                                        unsigned flags) const {
  GraphNode::doToVelocyPack(nodes, flags);  // call base class method
  nodes.add(StaticStrings::GraphQueryShortestPathType,
            VPackValue(arangodb::graph::PathType::toString(_pathType)));

  // Out variables
  if (usesPathOutVariable()) {
    nodes.add(VPackValue("pathOutVariable"));
    pathOutVariable().toVelocyPack(nodes);
  }

  // In variables
  if (usesStartInVariable()) {
    nodes.add(VPackValue("startInVariable"));
    startInVariable().toVelocyPack(nodes);
  } else {
    nodes.add("startVertexId", VPackValue(_startVertexId));
  }

  if (usesTargetInVariable()) {
    nodes.add(VPackValue("targetInVariable"));
    targetInVariable().toVelocyPack(nodes);
  } else {
    nodes.add("targetVertexId", VPackValue(_targetVertexId));
  }

  // Filter Conditions
  TRI_ASSERT(_fromCondition != nullptr);
  nodes.add(VPackValue("fromCondition"));
  _fromCondition->toVelocyPack(nodes, flags);

  TRI_ASSERT(_toCondition != nullptr);
  nodes.add(VPackValue("toCondition"));
  _toCondition->toVelocyPack(nodes, flags);

  if (!_globalEdgeConditions.empty()) {
    nodes.add(VPackValue("globalEdgeConditions"));
    nodes.openArray();
    for (auto const& it : _globalEdgeConditions) {
      it->toVelocyPack(nodes, flags);
    }
    nodes.close();
  }

  if (!_globalVertexConditions.empty()) {
    nodes.add(VPackValue("globalVertexConditions"));
    nodes.openArray();
    for (auto const& it : _globalVertexConditions) {
      it->toVelocyPack(nodes, flags);
    }
    nodes.close();
  }

  if (_distributeVariable != nullptr) {
    nodes.add(VPackValue("distributeVariable"));
    _distributeVariable->toVelocyPack(nodes);
  }
}

// Provider is expected to be:
// 1. TracedKPathEnumerator<SingleServerProvider<SingleServerProviderStep>>
// 2. or SingleServerProvider<SingleServerProviderStep>
// 3. or ClusterProvider<ClusterProviderStep>

template<typename KPath, typename Provider, typename ProviderOptions>
std::unique_ptr<ExecutionBlock> EnumeratePathsNode::_makeExecutionBlockImpl(
    ShortestPathOptions* opts, ProviderOptions forwardProviderOptions,
    ProviderOptions backwardProviderOptions,
    arangodb::graph::TwoSidedEnumeratorOptions enumeratorOptions,
    PathValidatorOptions validatorOptions, const RegisterId& outputRegister,
    ExecutionEngine& engine, InputVertex sourceInput, InputVertex targetInput,
    RegisterInfos registerInfos) const {
  auto kPathUnique = std::make_unique<KPath>(
      Provider{opts->query(), std::move(forwardProviderOptions),
               opts->query().resourceMonitor()},
      Provider{opts->query(), std::move(backwardProviderOptions),
               opts->query().resourceMonitor()},
      std::move(enumeratorOptions), std::move(validatorOptions),
      opts->query().resourceMonitor());

  auto executorInfos = EnumeratePathsExecutorInfos(
      outputRegister, engine.getQuery(), std::move(kPathUnique),
      std::move(sourceInput), std::move(targetInput));

  return std::make_unique<ExecutionBlockImpl<EnumeratePathsExecutor<KPath>>>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
};

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> EnumeratePathsNode::createBlock(
    ExecutionEngine& engine) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  RegIdSet inputRegisters;
  if (usesStartInVariable()) {
    inputRegisters.emplace(variableToRegisterId(&startInVariable()));
  }
  if (usesTargetInVariable()) {
    inputRegisters.emplace(variableToRegisterId(&targetInVariable()));
  }

  TRI_ASSERT(usesPathOutVariable());  // This node always produces the path!
  auto outputRegister = variableToRegisterId(&pathOutVariable());
  auto outputRegisters = RegIdSet{outputRegister};

  auto registerInfos = createRegisterInfos(std::move(inputRegisters),
                                           std::move(outputRegisters));

  auto opts = static_cast<ShortestPathOptions*>(options());

  GraphNode::InputVertex sourceInput = ::prepareVertexInput(this, false);
  GraphNode::InputVertex targetInput = ::prepareVertexInput(this, true);

  // Can only be specified in ShortestPathNode.cpp - not allowed here
  TRI_ASSERT(pathType() != arangodb::graph::PathType::Type::ShortestPath);

  arangodb::graph::TwoSidedEnumeratorOptions enumeratorOptions{
      opts->minDepth, opts->maxDepth, pathType()};
  PathValidatorOptions validatorOptions(opts->tmpVar(),
                                        opts->getExpressionCtx());

  // The _allVerticesExpression is a condition that has to hold for all vertices
  // on a result path.
  if (opts->_allVerticesExpression != nullptr) {
    auto allVerticesExpression =
        opts->_allVerticesExpression->clone(_plan->getAst());
    validatorOptions.setAllVerticesExpression(std::move(allVerticesExpression));
  }

  if (opts->_allEdgesExpression != nullptr) {
    auto allEdgesExpression = opts->_allEdgesExpression->clone(_plan->getAst());
    validatorOptions.setAllEdgesExpression(std::move(allEdgesExpression));
  }

  if (!ServerState::instance()->isCoordinator()) {
    // Create IndexAccessor for BaseProviderOptions (TODO: Location need to
    // be changed in the future) create BaseProviderOptions

    std::pair<std::vector<IndexAccessor>,
              std::unordered_map<uint64_t, std::vector<IndexAccessor>>>
        usedIndexes{};
    usedIndexes.first = buildUsedIndexes();

    std::pair<std::vector<IndexAccessor>,
              std::unordered_map<uint64_t, std::vector<IndexAccessor>>>
        reversedUsedIndexes{};
    reversedUsedIndexes.first = buildReverseUsedIndexes();

    // TODO [GraphRefactor]: Clean this up (de-duplicate with
    // SmartGraphEngine)
    SingleServerBaseProviderOptions forwardProviderOptions(
        opts->tmpVar(), std::move(usedIndexes), opts->getExpressionCtx(), {},
        opts->collectionToShard(), opts->getVertexProjections(),
        opts->getEdgeProjections(), opts->produceVertices());

    SingleServerBaseProviderOptions backwardProviderOptions(
        opts->tmpVar(), std::move(reversedUsedIndexes),
        opts->getExpressionCtx(), {}, opts->collectionToShard(),
        opts->getVertexProjections(), opts->getEdgeProjections(),
        opts->produceVertices());

    using Provider = SingleServerProvider<SingleServerProviderStep>;
    if (opts->query().queryOptions().getTraversalProfileLevel() ==
        TraversalProfileLevel::None) {
      switch (pathType()) {
        case arangodb::graph::PathType::Type::KPaths:
          return _makeExecutionBlockImpl<KPathEnumerator<Provider>, Provider,
                                         SingleServerBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        case arangodb::graph::PathType::Type::AllShortestPaths:
          return _makeExecutionBlockImpl<AllShortestPathsEnumerator<Provider>,
                                         Provider,
                                         SingleServerBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        case arangodb::graph::PathType::Type::KShortestPaths:
          enumeratorOptions.setMinDepth(0);
          enumeratorOptions.setMaxDepth(std::numeric_limits<size_t>::max());

          if (!opts->useWeight()) {
            // Non-Weighted Variant
            return _makeExecutionBlockImpl<KShortestPathsEnumerator<Provider>,
                                           Provider,
                                           SingleServerBaseProviderOptions>(
                opts, std::move(forwardProviderOptions),
                std::move(backwardProviderOptions), enumeratorOptions,
                validatorOptions, outputRegister, engine, sourceInput,
                targetInput, registerInfos);
          } else {
            // Weighted Variant
            double defaultWeight = opts->getDefaultWeight();
            std::string weightAttribute = opts->getWeightAttribute();
            forwardProviderOptions.setWeightEdgeCallback(
                [weightAttribute = weightAttribute, defaultWeight](
                    double previousWeight, VPackSlice edge) -> double {
                  auto const weight =
                      arangodb::basics::VelocyPackHelper::getNumericValue<
                          double>(edge, weightAttribute, defaultWeight);
                  if (weight < 0.) {
                    THROW_ARANGO_EXCEPTION(
                        TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
                  }

                  return previousWeight + weight;
                });
            backwardProviderOptions.setWeightEdgeCallback(
                [weightAttribute = weightAttribute, defaultWeight](
                    double previousWeight, VPackSlice edge) -> double {
                  auto const weight =
                      arangodb::basics::VelocyPackHelper::getNumericValue<
                          double>(edge, weightAttribute, defaultWeight);
                  if (weight < 0.) {
                    THROW_ARANGO_EXCEPTION(
                        TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
                  }

                  return previousWeight + weight;
                });

            return _makeExecutionBlockImpl<
                WeightedKShortestPathsEnumerator<Provider>, Provider,
                SingleServerBaseProviderOptions>(
                opts, std::move(forwardProviderOptions),
                std::move(backwardProviderOptions), enumeratorOptions,
                validatorOptions, outputRegister, engine, sourceInput,
                targetInput, registerInfos);
          }

        default:
          ADB_PROD_ASSERT(false)
              << "unknown PathType in EnumeratePathsNode::createBlock";
          std::abort();
      }
    } else {
      switch (pathType()) {
        case arangodb::graph::PathType::Type::KPaths:
          return _makeExecutionBlockImpl<TracedKPathEnumerator<Provider>,
                                         ProviderTracer<Provider>,
                                         SingleServerBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        case arangodb::graph::PathType::Type::AllShortestPaths:
          return _makeExecutionBlockImpl<
              TracedAllShortestPathsEnumerator<Provider>,
              ProviderTracer<Provider>, SingleServerBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        case arangodb::graph::PathType::Type::KShortestPaths:
          // TODO: deduplicate with non-traced variant. Too much code
          //  duplication right now.
          enumeratorOptions.setMinDepth(0);
          enumeratorOptions.setMaxDepth(std::numeric_limits<size_t>::max());

          if (!opts->useWeight()) {
            // Non-Weighted Variant
            return _makeExecutionBlockImpl<
                TracedKShortestPathsEnumerator<Provider>,
                ProviderTracer<Provider>, SingleServerBaseProviderOptions>(
                opts, std::move(forwardProviderOptions),
                std::move(backwardProviderOptions), enumeratorOptions,
                validatorOptions, outputRegister, engine, sourceInput,
                targetInput, registerInfos);
          } else {
            // Weighted Variant
            double defaultWeight = opts->getDefaultWeight();
            std::string weightAttribute = opts->getWeightAttribute();
            forwardProviderOptions.setWeightEdgeCallback(
                [weightAttribute = weightAttribute, defaultWeight](
                    double previousWeight, VPackSlice edge) -> double {
                  auto const weight =
                      arangodb::basics::VelocyPackHelper::getNumericValue<
                          double>(edge, weightAttribute, defaultWeight);
                  if (weight < 0.) {
                    THROW_ARANGO_EXCEPTION(
                        TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
                  }

                  return previousWeight + weight;
                });
            backwardProviderOptions.setWeightEdgeCallback(
                [weightAttribute = weightAttribute, defaultWeight](
                    double previousWeight, VPackSlice edge) -> double {
                  auto const weight =
                      arangodb::basics::VelocyPackHelper::getNumericValue<
                          double>(edge, weightAttribute, defaultWeight);
                  if (weight < 0.) {
                    THROW_ARANGO_EXCEPTION(
                        TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
                  }

                  return previousWeight + weight;
                });

            return _makeExecutionBlockImpl<
                WeightedKShortestPathsEnumerator<Provider>, Provider,
                SingleServerBaseProviderOptions>(
                opts, std::move(forwardProviderOptions),
                std::move(backwardProviderOptions), enumeratorOptions,
                validatorOptions, outputRegister, engine, sourceInput,
                targetInput, registerInfos);
          }
        default:
          ADB_PROD_ASSERT(false)
              << "unknown PathType in EnumeratePathsNode::createBlock";
          std::abort();
      }
    }
  } else {
    auto cache = std::make_shared<RefactoredClusterTraverserCache>(
        opts->query().resourceMonitor());
    ClusterBaseProviderOptions forwardProviderOptions(cache, engines(), false,
                                                      opts->produceVertices());
    ClusterBaseProviderOptions backwardProviderOptions(cache, engines(), true,
                                                       opts->produceVertices());

    using ClusterProvider = ClusterProvider<ClusterProviderStep>;
    switch (pathType()) {
      case arangodb::graph::PathType::Type::KPaths:
        return _makeExecutionBlockImpl<KPathEnumerator<ClusterProvider>,
                                       ClusterProvider,
                                       ClusterBaseProviderOptions>(
            opts, std::move(forwardProviderOptions),
            std::move(backwardProviderOptions), enumeratorOptions,
            validatorOptions, outputRegister, engine, sourceInput, targetInput,
            registerInfos);
      case arangodb::graph::PathType::Type::AllShortestPaths:
        return _makeExecutionBlockImpl<
            AllShortestPathsEnumerator<ClusterProvider>, ClusterProvider,
            ClusterBaseProviderOptions>(opts, std::move(forwardProviderOptions),
                                        std::move(backwardProviderOptions),
                                        enumeratorOptions, validatorOptions,
                                        outputRegister, engine, sourceInput,
                                        targetInput, registerInfos);
      case arangodb::graph::PathType::Type::KShortestPaths:
        // TODO: deduplicate with SingleServer && SingleServer non-traced
        //  variant. Too much code duplication right now.
        enumeratorOptions.setMinDepth(0);
        enumeratorOptions.setMaxDepth(std::numeric_limits<size_t>::max());

        if (!opts->useWeight()) {
          // Non-Weighted Variant
          return _makeExecutionBlockImpl<
              KShortestPathsEnumerator<ClusterProvider>, ClusterProvider,
              ClusterBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        } else {
          // Weighted Variant
          double defaultWeight = opts->getDefaultWeight();
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

          return _makeExecutionBlockImpl<
              WeightedKShortestPathsEnumerator<ClusterProvider>,
              ClusterProvider, ClusterBaseProviderOptions>(
              opts, std::move(forwardProviderOptions),
              std::move(backwardProviderOptions), enumeratorOptions,
              validatorOptions, outputRegister, engine, sourceInput,
              targetInput, registerInfos);
        }
      default:
        ADB_PROD_ASSERT(false)
            << "unknown PathType in EnumeratePathsNode::createBlock";
    }
  }

  ADB_PROD_ASSERT(false)
      << "unknown PathType in EnumeratePathsNode::createBlock";
  std::abort();
}

ExecutionNode* EnumeratePathsNode::clone(ExecutionPlan* plan,
                                         bool withDependencies,
                                         bool withProperties) const {
  TRI_ASSERT(!_optionsBuilt);
  auto oldOpts = static_cast<ShortestPathOptions*>(options());
  std::unique_ptr<BaseOptions> tmp =
      std::make_unique<ShortestPathOptions>(*oldOpts);
  auto c = std::make_unique<EnumeratePathsNode>(
      plan, _id, _vocbase, _pathType, _edgeColls, _vertexColls,
      _defaultDirection, _directions, _inStartVariable, _startVertexId,
      _inTargetVariable, _targetVertexId, std::move(tmp), _graphObj,
      _distributeVariable);

  enumeratePathsCloneHelper(*plan, *c, withProperties);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

void EnumeratePathsNode::enumeratePathsCloneHelper(ExecutionPlan& plan,
                                                   EnumeratePathsNode& c,
                                                   bool withProperties) const {
  graphCloneHelper(plan, c, withProperties);
  if (usesPathOutVariable()) {
    auto pathOutVariable = _pathOutVariable;
    if (withProperties) {
      pathOutVariable =
          plan.getAst()->variables()->createVariable(pathOutVariable);
    }
    TRI_ASSERT(pathOutVariable != nullptr);
    c.setPathOutput(pathOutVariable);
  }

  // Temporary Filter Objects
  c._tmpObjVariable = _tmpObjVariable;
  c._tmpObjVarNode = _tmpObjVarNode;
  c._tmpIdNode = _tmpIdNode;

  // Filter Condition Parts
  c._fromCondition = _fromCondition->clone(_plan->getAst());
  c._toCondition = _toCondition->clone(_plan->getAst());

  // Global Vertex/Edge conditions
  c._globalEdgeConditions.insert(c._globalEdgeConditions.end(),
                                 _globalEdgeConditions.begin(),
                                 _globalEdgeConditions.end());
  c._globalVertexConditions.insert(c._globalVertexConditions.end(),
                                   _globalVertexConditions.begin(),
                                   _globalVertexConditions.end());
}

void EnumeratePathsNode::replaceVariables(
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
}

/// @brief getVariablesSetHere
std::vector<Variable const*> EnumeratePathsNode::getVariablesSetHere() const {
  std::vector<Variable const*> vars;
  TRI_ASSERT(_pathOutVariable != nullptr);
  vars.emplace_back(_pathOutVariable);
  return vars;
}

/// @brief getVariablesUsedHere, modifying the set in-place
void EnumeratePathsNode::getVariablesUsedHere(VarSet& vars) const {
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

std::vector<arangodb::graph::IndexAccessor>
EnumeratePathsNode::buildUsedIndexes() const {
  return buildIndexes(/*reverse*/ false);
}

std::vector<arangodb::graph::IndexAccessor>
EnumeratePathsNode::buildReverseUsedIndexes() const {
  return buildIndexes(/*reverse*/ true);
}

std::vector<arangodb::graph::IndexAccessor> EnumeratePathsNode::buildIndexes(
    bool reverse) const {
  size_t numEdgeColls = _edgeColls.size();
  constexpr bool onlyEdgeIndexes = true;

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
    auto& trx = plan()->getAst()->query().trxForOptimization();
    bool res = aql::utils::getBestIndexHandleForFilterCondition(
        trx, *_edgeColls[i], clonedCondition, options()->tmpVar(),
        itemsInCollection, aql::IndexHint(), indexToUse, onlyEdgeIndexes);
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

size_t EnumeratePathsNode::getMemoryUsedBytes() const { return sizeof(*this); }

void EnumeratePathsNode::prepareOptions() {
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
                            /*onlyEdgeIndexes*/ false, dir);
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

  if (!_globalVertexConditions.empty()) {
    auto cond =
        _plan->getAst()->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    for (auto const& it : _globalVertexConditions) {
      cond->addMember(it);
    }
    opts->_allVerticesExpression = std::make_unique<Expression>(ast, cond);
  }

  if (!_globalEdgeConditions.empty()) {
    auto cond =
        _plan->getAst()->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    for (auto const& it : _globalEdgeConditions) {
      cond->addMember(it);
    }
    opts->_allEdgesExpression = std::make_unique<Expression>(ast, cond);
  }

  // If we use the path output the cache should activate document
  // caching otherwise it is not worth it.
  if (!ServerState::instance()->isCoordinator()) {
    _options->activateCache(false, nullptr);
  }
  _optionsBuilt = true;
}

auto EnumeratePathsNode::registerGlobalVertexCondition(AstNode const* condition)
    -> void {
  _globalVertexConditions.emplace_back(condition);
}

auto EnumeratePathsNode::registerGlobalEdgeCondition(AstNode const* condition)
    -> void {
  _globalEdgeConditions.emplace_back(condition);
}

auto EnumeratePathsNode::options() const -> graph::ShortestPathOptions* {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* opts = dynamic_cast<ShortestPathOptions*>(GraphNode::options());
  TRI_ASSERT((GraphNode::options() == nullptr) == (opts == nullptr));
#else
  auto* opts = static_cast<ShortestPathOptions*>(GraphNode::options());
#endif
  return opts;
}
