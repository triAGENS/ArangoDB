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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ExecutionPlan.h"

#include "Aql/Aggregator.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/CollectOptions.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionNode/CalculationNode.h"
#include "Aql/ExecutionNode/CollectNode.h"
#include "Aql/ExecutionNode/EnumerateCollectionNode.h"
#include "Aql/ExecutionNode/EnumerateListNode.h"
#include "Aql/ExecutionNode/EnumerateNearVectorNode.h"
#include "Aql/ExecutionNode/EnumeratePathsNode.h"
#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ExecutionNode/FilterNode.h"
#include "Aql/ExecutionNode/IResearchViewNode.h"
#include "Aql/ExecutionNode/IndexNode.h"
#include "Aql/ExecutionNode/InsertNode.h"
#include "Aql/ExecutionNode/LimitNode.h"
#include "Aql/ExecutionNode/NoResultsNode.h"
#include "Aql/ExecutionNode/RemoveNode.h"
#include "Aql/ExecutionNode/ReplaceNode.h"
#include "Aql/ExecutionNode/ReturnNode.h"
#include "Aql/ExecutionNode/ShortestPathNode.h"
#include "Aql/ExecutionNode/SingletonNode.h"
#include "Aql/ExecutionNode/SortNode.h"
#include "Aql/ExecutionNode/SubqueryNode.h"
#include "Aql/ExecutionNode/TraversalNode.h"
#include "Aql/ExecutionNode/UpdateNode.h"
#include "Aql/ExecutionNode/UpsertNode.h"
#include "Aql/ExecutionNode/WindowNode.h"
#include "Aql/Expression.h"
#include "Aql/Function.h"
#include "Aql/IndexHint.h"
#include "Aql/NodeFinder.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/VarUsageFinder.h"
#include "Aql/Variable.h"
#include "Aql/WalkerWorker.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Containers/SmallVector.h"
#include "Graph/PathType.h"
#include "Graph/ShortestPathOptions.h"
#include "Graph/TraverserOptions.h"
#include "Logger/LoggerStream.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Utils/OperationOptions.h"
#include "VocBase/AccessMode.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Iterator.h>

#include <initializer_list>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace {

static constexpr std::initializer_list<ExecutionNode::NodeType>
    indexHintCheckTypes{
        ExecutionNode::ENUMERATE_COLLECTION,
#ifdef EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
        ExecutionNode::TRAVERSAL,
        ExecutionNode::ENUMERATE_PATHS,
#endif
    };

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
/// @brief validate the counters of the plan
struct NodeCounter final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
  std::array<uint32_t, ExecutionNode::MAX_NODE_TYPE_VALUE> counts;
  std::unordered_set<ExecutionNode const*> seen;

  NodeCounter() : counts{} {}

  bool enterSubquery(ExecutionNode*, ExecutionNode*) override final {
    return true;
  }

  void after(ExecutionNode* en) override final {
    if (seen.find(en) == seen.end()) {
      // There is a chance that we ahve the same node twice
      // if we have multiple streams leading to it (e.g. Distribute)
      counts[en->getType()]++;
      seen.emplace(en);
    }
  }

  bool done(ExecutionNode* en) override final {
    if (en->getType() == ExecutionNode::MUTEX && seen.find(en) != seen.end()) {
      return true;
    }
    if (!arangodb::ServerState::instance()->isDBServer() ||
        (en->getType() != ExecutionNode::REMOTE &&
         en->getType() != ExecutionNode::SCATTER &&
         en->getType() != ExecutionNode::DISTRIBUTE)) {
      return WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique>::done(en);
    }
    return false;
  }
};
#endif

uint64_t checkDepthValue(AstNode const* node) {
  if (!node->isNumericValue()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }
  double v = node->getDoubleValue();
  if (v > static_cast<double>(INT64_MAX)) {
    // we cannot safely represent this value in an int64_t.
    // which is already far bigger than we will ever traverse, so
    // we can safely abort here and not care about uint64_t.
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }

  double intpart;
  if (modf(v, &intpart) != 0.0 || v < 0.0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }
  return static_cast<uint64_t>(v);
}

std::pair<uint64_t, uint64_t> getMinMaxDepths(AstNode const* steps) {
  uint64_t minDepth = 0;
  uint64_t maxDepth = 0;
  bool invalidDepth = false;

  if (steps->isNumericValue()) {
    // Check if a double value is integer
    minDepth = checkDepthValue(steps);
    maxDepth = checkDepthValue(steps);
  } else if (steps->type == NODE_TYPE_RANGE) {
    // Range depth
    minDepth = checkDepthValue(steps->getMember(0));
    maxDepth = checkDepthValue(steps->getMember(1));
  } else {
    invalidDepth = true;
  }

  if (maxDepth < minDepth) {
    invalidDepth = true;
  }

  if (invalidDepth) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }

  return {minDepth, maxDepth};
}

void parseGraphCollectionRestriction(Ast* ast, std::string_view typeName,
                                     std::vector<std::string>& collections,
                                     AstNode const* src) {
  auto addCollection = [&collections, typeName, ast](std::string&& name) {
    // throws if data source cannot be found
    ast->createNodeDataSource(ast->query().resolver(), name,
                              AccessMode::Type::READ, true, true);

    // now get the collection and inspect its type
    Collection const* collection = ast->query().collections().get(name);
    TRI_ASSERT(collection != nullptr);
    if (collection == nullptr) {
      // should not happen
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
          absl::StrCat("data source for '", typeName,
                       "' option must be a collection"));
    }

    TRI_col_type_e type = TRI_COL_TYPE_DOCUMENT;
    try {
      // throws when called on a view
      type = collection->type();
    } catch (...) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
          absl::StrCat("data source type for '", typeName,
                       "' option must be a collection"));
    }

    if (typeName == "edgeCollections") {
      if (type != TRI_COL_TYPE_EDGE) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
            absl::StrCat("data source type for '", typeName,
                         "' option must be an edge collection"));
      }
    }

    collections.emplace_back(std::move(name));
  };

  if (src->isStringValue()) {
    addCollection(src->getString());
  } else if (src->type == NODE_TYPE_ARRAY) {
    size_t const n = src->numMembers();
    collections.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      AstNode const* c = src->getMemberUnchecked(i);
      if (!c->isStringValue()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_BAD_PARAMETER,
            absl::StrCat(
                "collection restrictions option for '", typeName,
                "' must be either a string or an array of collection names"));
      }
      addCollection(c->getString());
    }
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "collection restrictions option must be either a string or an array of "
        "collection names");
  }
}

ResultT<size_t> parseMaxProjections(AstNode const* value) {
  int64_t maxProjections = -1;

  if (value->isNumericValue()) {
    if (value->isIntValue()) {
      maxProjections = value->getIntValue();
    } else if (value->isDoubleValue()) {
      maxProjections = static_cast<int64_t>(value->getDoubleValue());
    }
  }
  if (maxProjections >= 0) {
    // got a valid value
    return static_cast<size_t>(maxProjections);
  }
  return Result{TRI_ERROR_BAD_PARAMETER};
}

std::unique_ptr<graph::BaseOptions> createTraversalOptions(
    Ast* ast, AstNode const* direction, AstNode const* optionsNode) {
  TRI_ASSERT(direction != nullptr);
  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);

  // will throw on invalid depths
  auto [minDepth, maxDepth] = getMinMaxDepths(direction->getMember(1));

  aql::QueryContext& query = ast->query();
  auto options = std::make_unique<traverser::TraverserOptions>(query);
  options->minDepth = minDepth;
  options->maxDepth = maxDepth;

  // WTF this is the 10th place the deals with parsing of options
  // how do you even maintain that?
  if (optionsNode != nullptr && optionsNode->type == NODE_TYPE_OBJECT) {
    size_t n = optionsNode->numMembers();
    bool hasBFS = false;

    for (size_t i = 0; i < n; ++i) {
      auto member = optionsNode->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringView();
        auto value = member->getMember(0);
        TRI_ASSERT(value->isConstant());

        if (name == "bfs") {
          options->mode =
              value->isTrue()
                  ? arangodb::traverser::TraverserOptions::Order::BFS
                  : arangodb::traverser::TraverserOptions::Order::DFS;
          hasBFS = true;
        } else if (name == "uniqueVertices" && value->isStringValue()) {
          if (value->stringEqualsCaseInsensitive(
                  StaticStrings::GraphQueryPath)) {
            options->uniqueVertices =
                traverser::TraverserOptions::UniquenessLevel::PATH;
          } else if (value->stringEqualsCaseInsensitive(
                         StaticStrings::GraphQueryGlobal)) {
            options->uniqueVertices =
                traverser::TraverserOptions::UniquenessLevel::GLOBAL;
          }
        } else if (name == "uniqueEdges" && value->isStringValue()) {
          // path is the default
          if (value->stringEqualsCaseInsensitive(
                  StaticStrings::GraphQueryNone)) {
            options->uniqueEdges =
                traverser::TraverserOptions::UniquenessLevel::NONE;
          } else if (value->stringEqualsCaseInsensitive(
                         StaticStrings::GraphQueryGlobal)) {
            THROW_ARANGO_EXCEPTION_MESSAGE(
                TRI_ERROR_BAD_PARAMETER,
                "uniqueEdges: 'global' is not supported, "
                "due to otherwise unpredictable results. Use 'path' "
                "or 'none' instead");
          }
        } else if (name == "edgeCollections") {
          parseGraphCollectionRestriction(ast, name, options->edgeCollections,
                                          value);
        } else if (name == "vertexCollections") {
          parseGraphCollectionRestriction(ast, name, options->vertexCollections,
                                          value);
        } else if (name == StaticStrings::GraphQueryOrder && !hasBFS) {
          // dfs is the default
          if (value->stringEqualsCaseInsensitive(
                  StaticStrings::GraphQueryOrderBFS)) {
            options->mode = traverser::TraverserOptions::Order::BFS;
          } else if (value->stringEqualsCaseInsensitive(
                         StaticStrings::GraphQueryOrderWeighted)) {
            options->mode = traverser::TraverserOptions::Order::WEIGHTED;
          } else if (value->stringEqualsCaseInsensitive(
                         StaticStrings::GraphQueryOrderDFS)) {
            options->mode = traverser::TraverserOptions::Order::DFS;
          } else {
            THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                           "order: unknown order");
          }
        } else if (name == "defaultWeight" && value->isNumericValue()) {
          options->defaultWeight = value->getDoubleValue();
          if (options->defaultWeight < 0.) {
            THROW_ARANGO_EXCEPTION_MESSAGE(
                TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT,
                "negative default weight not allowed");
          }
        } else if (name == "weightAttribute" && value->isStringValue()) {
          options->weightAttribute = value->getString();
        } else if (name == "parallelism") {
          if (ast->canApplyParallelism()) {
            // parallelism is only used when there is no usage of V8 in the
            // query and if the query is not a modification query.
            options->setParallelism(Ast::validatedParallelism(value));
          }
        } else if (name == StaticStrings::MaxProjections) {
          auto maxProjections = parseMaxProjections(value);
          if (maxProjections.fail()) {
            // will raise a warning, which can optionally abort the query
            ExecutionPlan::invalidOptionAttribute(query, "invalid", "TRAVERSAL",
                                                  name);
          } else {
            options->setMaxProjections(maxProjections.get());
          }
        } else if (name == StaticStrings::IndexHintOptionForce) {
#ifdef EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
          // will be handled by the following handler for "indexHint"
#else
          // TODO: forceIndexHint is currently not supported for traversal index
          // hints
          ExecutionPlan::invalidOptionAttribute(ast->query(), "unknown",
                                                "TRAVERSAL", name);
#endif
        } else if (name == StaticStrings::IndexHintOption) {
          options->setHint(
              IndexHint(ast->query(), optionsNode, IndexHint::FromTraversal{}));
        } else if (name == arangodb::StaticStrings::UseCache) {
          if (value->isBoolValue()) {
            options->setUseCache(value->getBoolValue());
          }
        } else {
          ExecutionPlan::invalidOptionAttribute(ast->query(), "unknown",
                                                "TRAVERSAL", name);
        }
      }
    }
  }

  if (options->uniqueVertices ==
          arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL &&
      !(options->isUniqueGlobalVerticesAllowed())) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "uniqueVertices: 'global' is only "
                                   "supported, with order: bfs|weighted due to "
                                   "otherwise unpredictable results.");
  }

  return options;
}

std::unique_ptr<graph::BaseOptions> createPathsQueryOptions(
    Ast* ast, AstNode const* direction, AstNode const* optionsNode,
    arangodb::graph::PathType::Type type) {
  TRI_ASSERT(direction != nullptr);
  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);

  // will throw on invalid depths
  auto [minDepth, maxDepth] = getMinMaxDepths(direction->getMember(1));

  aql::QueryContext& query = ast->query();
  auto options = std::make_unique<graph::ShortestPathOptions>(query);
  options->setMinDepth(minDepth);
  options->setMaxDepth(maxDepth);

  if (optionsNode != nullptr && optionsNode->type == NODE_TYPE_OBJECT) {
    size_t n = optionsNode->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = optionsNode->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringView();
        auto value = member->getMember(0);

        TRI_ASSERT(value->isConstant());

        if (name == "weightAttribute" && value->isStringValue()) {
          options->setWeightAttribute(value->getString());
        } else if (name == "defaultWeight" && value->isNumericValue()) {
          options->setDefaultWeight(value->getDoubleValue());
        } else if (name == StaticStrings::IndexHintOptionForce) {
#ifdef EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
          // will be handled by the following handler for "indexHint"
#else
          // TODO: index hints are currently unsupported for paths queries
          ExecutionPlan::invalidOptionAttribute(
              ast->query(), "unknown",
              arangodb::graph::PathType::toString(type), name);
#endif
        } else if (name == StaticStrings::IndexHintOption) {
#ifdef EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
          // TODO: enable this code once the index code for paths queries
          // support multi-key indexes and conditions
          options->setHint(IndexHint(ast->query(), optionsNode,
                                     IndexHint::FromPathsQuery{}));
#else
          // TODO: index hints are currently unsupported for paths queries
          ExecutionPlan::invalidOptionAttribute(
              ast->query(), "unknown",
              arangodb::graph::PathType::toString(type), name);
#endif
        } else if (name == arangodb::StaticStrings::UseCache) {
          if (value->isBoolValue()) {
            options->setUseCache(value->getBoolValue());
          }
        } else if (name == arangodb::StaticStrings::Algorithm) {
          if (value->isStringValue()) {
            options->setAlgorithm(value->getStringValue());
          }
        } else {
          ExecutionPlan::invalidOptionAttribute(
              ast->query(), "unknown",
              arangodb::graph::PathType::toString(type), name);
        }
      }
    }
  }

  return options;
}

// extract "maxProjections" and "useCache" from FOR options
void setForOptions(QueryContext& query, AstNode const* node,
                   DocumentProducingNode* dn) {
  // default value
  dn->setMaxProjections(DocumentProducingNode::kMaxProjections);

  if (node != nullptr && node->type == AstNodeType::NODE_TYPE_OBJECT) {
    for (size_t i = 0; i < node->numMembers(); i++) {
      AstNode const* child = node->getMember(i);

      if (child->type == AstNodeType::NODE_TYPE_OBJECT_ELEMENT) {
        TRI_ASSERT(child->numMembers() > 0);
        std::string_view name(child->getStringView());
        if (name == arangodb::StaticStrings::MaxProjections) {
          auto maxProjections = parseMaxProjections(child->getMember(0));
          if (maxProjections.fail()) {
            // will raise a warning, which can optionally abort the query
            ExecutionPlan::invalidOptionAttribute(query, "invalid", "FOR",
                                                  name);
          } else {
            dn->setMaxProjections(maxProjections.get());
          }
        } else if (name == arangodb::StaticStrings::UseCache) {
          AstNode const* value = child->getMember(0);

          if (value->isBoolValue()) {
            dn->setUseCache(value->getBoolValue());
          } else {
            // will raise a warning, which can optionally abort the query
            ExecutionPlan::invalidOptionAttribute(query, "invalid", "FOR",
                                                  name);
          }
        }
      }
    }
  }
}

std::unique_ptr<Expression> createPruneExpression(ExecutionPlan* plan, Ast* ast,
                                                  AstNode* node) {
  if (node->type == NODE_TYPE_NOP) {
    return nullptr;
  }
  return std::make_unique<Expression>(ast, node);
}

}  // namespace

/// @brief create the plan
ExecutionPlan::ExecutionPlan(Ast* ast, bool trackMemoryUsage)
    : _root(nullptr),
      _trackMemoryUsage(trackMemoryUsage),
      _planValid(true),
      _varUsageComputed(false),
      _nestingLevel(0),
      _nextId(0),
      _ast(ast),
      _lastLimitNode(nullptr),
      _typeCounts{},
      _asyncPrefetchNodes(0) {}

/// @brief destroy the plan, frees all assigned nodes
ExecutionPlan::~ExecutionPlan() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // On coordinator there are temporary nodes injected into the Plan, that are
  // NOT part of the full execution tree. This is in order to reuse a lot of the
  // nodes for each DBServer instead of having all nodes copy everything once
  // again.
  if (_root != nullptr && _planValid &&
      !arangodb::ServerState::instance()->isCoordinator()) {
    try {
      // count the actual number of nodes in the plan
      ::NodeCounter counter;
      _root->walk(counter);

      // and compare it to the number of nodes we have in our counters array
      size_t j = 0;
      for (auto const& it : _typeCounts) {
        TRI_ASSERT(counter.counts[j] == it);
        ++j;
      }
    } catch (...) {
      // should not happen...
    }
  }
#endif

  if (_trackMemoryUsage) {
    // only track memory usage here and access ast/query if we are allowed to do
    // so. this can be inherently unsafe from within the gtest unit tests, so it
    // is protected by an option here
    for (auto const& [id, node] : _ids) {
      _ast->query().resourceMonitor().decreaseMemoryUsage(
          node->getMemoryUsedBytes());
    }
  }

  for (auto& x : _ids) {
    delete x.second;
  }
}

/// @brief create an execution plan from an AST
/*static*/ std::unique_ptr<ExecutionPlan> ExecutionPlan::instantiateFromAst(
    Ast* ast, bool trackMemoryUsage) {
  TRI_ASSERT(ast != nullptr);

  auto root = ast->root();
  TRI_ASSERT(root != nullptr);
  TRI_ASSERT(root->type == NODE_TYPE_ROOT);

  auto plan = std::make_unique<ExecutionPlan>(ast, trackMemoryUsage);

  plan->_root = plan->fromNode(root);

  // set fullCount flag for last LIMIT node on main level
  if (plan->_lastLimitNode != nullptr &&
      ast->query().queryOptions().fullCount) {
    ExecutionNode::castTo<LimitNode*>(plan->_lastLimitNode)->setFullCount();
  }

  // set count flag for final RETURN node
  if (plan->_root->getType() == ExecutionNode::RETURN) {
    ExecutionNode::castTo<ReturnNode*>(plan->_root)->setCount();
  }

  plan->findVarUsage();

  return plan;
}

/// @brief increase number of async prefetch nodes
void ExecutionPlan::increaseAsyncPrefetchNodes() noexcept {
  ++_asyncPrefetchNodes;
}

/// @brief decrease number of async prefetch nodes
void ExecutionPlan::decreaseAsyncPrefetchNodes() noexcept {
  TRI_ASSERT(_asyncPrefetchNodes > 0);
  --_asyncPrefetchNodes;
}

size_t ExecutionPlan::asyncPrefetchNodes() const noexcept {
  return _asyncPrefetchNodes;
}

void ExecutionPlan::invalidOptionAttribute(QueryContext& query,
                                           std::string_view errorReason,
                                           std::string_view operationName,
                                           std::string_view name) {
  // if warnings are converted into errors, adding this warning will
  // abort the query
  query.warnings().registerWarning(
      TRI_ERROR_QUERY_INVALID_OPTIONS_ATTRIBUTE,
      absl::StrCat("usage of ", errorReason, " OPTIONS attribute '", name,
                   "' in ", operationName, " statement"));
}

bool ExecutionPlan::contains(ExecutionNode::NodeType type) const {
  TRI_ASSERT(_varUsageComputed);
  return _typeCounts[type] > 0;
}

/// @brief increase the node counter for the type
void ExecutionPlan::increaseCounter(ExecutionNode const& node) noexcept {
  auto const type = node.getType();
  ++_typeCounts[type];

  // Tracking forced index hints left to use.
  // This could be only collection nodes. IndexNodes
  // are created by optimizer as a result of applying index
  // and corresponding hint if any.
  if (type == ExecutionNode::ENUMERATE_COLLECTION) {
    EnumerateCollectionNode const* en =
        ExecutionNode::castTo<EnumerateCollectionNode const*>(&node);
    auto const& hint = en->hint();
    _hasForcedIndexHints |= hint.isSet() && hint.isForced();
#if EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
  } else if (type == ExecutionNode::TRAVERSAL) {
    // TODO: enable this code when we fully support forced index hints for
    // traversals. then also add other graph node types here.
    TraversalNode const* en =
        ExecutionNode::castTo<TraversalNode const*>(&node);
    auto const& hint = en->hint();
    _hasForcedIndexHints |= hint.isSet() && hint.isForced();
  } else if (type == ExecutionNode::ENUMERATE_PATHS) {
    // TODO: enable this code when we fully support forced index hints for
    // traversals. then also add other graph node types here.
    EnumeratePathsNode const* en =
        ExecutionNode::castTo<EnumeratePathsNode const*>(&node);
    auto const& hint = en->hint();
    _hasForcedIndexHints |= hint.isSet() && hint.isForced();
#endif
  }
}

/// @brief process the list of collections in a VelocyPack
void ExecutionPlan::getCollectionsFromVelocyPack(aql::Collections& colls,
                                                 velocypack::Slice slice) {
  VPackSlice collectionsSlice = slice;
  if (slice.isObject()) {
    collectionsSlice = slice.get("collections");
  }

  if (!collectionsSlice.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "json node \"collections\" not found or not an array");
  }

  for (auto collection : VPackArrayIterator(collectionsSlice)) {
    colls.add(
        basics::VelocyPackHelper::checkAndGetStringValue(collection, "name"),
        AccessMode::fromString(
            arangodb::basics::VelocyPackHelper::checkAndGetStringValue(
                collection, "type")),
        aql::Collection::Hint::Shard);
  }
}

/// @brief process a list of views in a VelocyPack
void ExecutionPlan::extendCollectionsByViewsFromVelocyPack(
    aql::Collections& colls, velocypack::Slice viewsSlice) {
  if (!viewsSlice.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "json for views is not an array");
  }

  for (auto view : VPackArrayIterator(viewsSlice)) {
    colls.add(basics::VelocyPackHelper::checkAndGetStringValue(view, "name"),
              AccessMode::fromString(
                  arangodb::basics::VelocyPackHelper::checkAndGetStringValue(
                      view, "type")),
              aql::Collection::Hint::None);
  }
}

/// @brief create an execution plan from VelocyPack
std::unique_ptr<ExecutionPlan> ExecutionPlan::instantiateFromVelocyPack(
    Ast* ast, velocypack::Slice slice, bool simpleSnippetFormat) {
  TRI_ASSERT(ast != nullptr);

  auto plan = std::make_unique<ExecutionPlan>(ast, /*trackMemoryUsage*/ true);
  plan->_root = plan->fromSlice(slice, simpleSnippetFormat);
  plan->setVarUsageComputed();

  if (!simpleSnippetFormat) {
    TRI_ASSERT(slice.isObject());

    if (auto rules = slice.get("rules"); rules.isArray()) {
      for (auto rule : VPackArrayIterator(rules)) {
        int ruleId = OptimizerRulesFeature::translateRule(rule.stringView());
        plan->_appliedRules.push_back(ruleId);
      }
    }

    if (auto apfn = slice.get("asyncPrefetchNodes"); apfn.isNumber<size_t>()) {
      plan->_asyncPrefetchNodes = apfn.getNumericValue<size_t>();
    }
  }

  return plan;
}

/// @brief clone an existing execution plan
std::unique_ptr<ExecutionPlan> ExecutionPlan::clone(Ast* ast) {
  TRI_ASSERT(ast != nullptr);

  auto plan = std::make_unique<ExecutionPlan>(ast, _trackMemoryUsage);
  plan->_nextId = _nextId;
  plan->_root = _root->clone(plan.get(), true);
  plan->_appliedRules = _appliedRules;
  plan->_disabledRules = _disabledRules;
  plan->_nestingLevel = _nestingLevel;
  plan->_asyncPrefetchNodes = _asyncPrefetchNodes;

  return plan;
}

/// @brief clone an existing execution plan
std::unique_ptr<ExecutionPlan> ExecutionPlan::clone() { return clone(_ast); }

// build flags for plan serialization
unsigned ExecutionPlan::buildSerializationFlags(
    bool verbose, bool includeInternals, bool explainRegisters) noexcept {
  unsigned flags = ExecutionNode::SERIALIZE_ESTIMATES;
  if (verbose) {
    flags |=
        ExecutionNode::SERIALIZE_PARENTS | ExecutionNode::SERIALIZE_FUNCTIONS;

    if (includeInternals) {
      flags |= ExecutionNode::SERIALIZE_DETAILS;
    }
  }

  if (explainRegisters) {
    flags |= ExecutionNode::SERIALIZE_REGISTER_INFORMATION;
  }

  return flags;
}

/// @brief export to VelocyPack
void ExecutionPlan::toVelocyPack(
    velocypack::Builder& builder, unsigned flags,
    std::function<void(velocypack::Builder&)> const& serializeQueryData) const {
  builder.openObject();

  builder.add(VPackValue("nodes"));
  _root->allToVelocyPack(builder, flags);

  // set up rules
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue("rules"));
  builder.openArray();
  for (int rule : _appliedRules) {
    std::string_view ruleName = OptimizerRulesFeature::translateRule(rule);
    if (!ruleName.empty()) {
      builder.add(VPackValue(ruleName));
    }
  }
  builder.close();

  builder.add("asyncPrefetchNodes", VPackValue(_asyncPrefetchNodes));

  // the following attributes are read by the explainer
  CostEstimate estimate = _root->getCost();
  builder.add("estimatedCost", VPackValue(estimate.estimatedCost));
  builder.add("estimatedNrItems", VPackValue(estimate.estimatedNrItems));

  serializeQueryData(builder);

  builder.close();
}

void ExecutionPlan::addAppliedRule(int level) {
  if (_appliedRules.empty() || _appliedRules.back() != level) {
    _appliedRules.emplace_back(level);
  }
}

bool ExecutionPlan::hasAppliedRule(int level) const noexcept {
  return std::any_of(_appliedRules.begin(), _appliedRules.end(),
                     [level](int l) { return l == level; });
}

void ExecutionPlan::enableRule(int rule) { _disabledRules.erase(rule); }

void ExecutionPlan::disableRule(int rule) { _disabledRules.emplace(rule); }

bool ExecutionPlan::isDisabledRule(int rule) const noexcept {
  return _disabledRules.contains(rule);
}

ExecutionNodeId ExecutionPlan::nextId() {
  _nextId = ExecutionNodeId{_nextId.id() + 1};
  return _nextId;
}

/// @brief get a node by its id
ExecutionNode* ExecutionPlan::getNodeById(ExecutionNodeId id) const {
  auto it = _ids.find(id);

  if (it != _ids.end()) {
    // node found
    return (*it).second;
  }

  // node unknown
  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL, absl::StrCat("node [", id.id(), "] wasn't found"));
}

std::unordered_map<ExecutionNodeId, ExecutionNode*> const&
ExecutionPlan::getNodesById() const {
  return _ids;
}

/// @brief creates a calculation node for an arbitrary expression
ExecutionNode* ExecutionPlan::createCalculation(Variable* out,
                                                AstNode const* expression,
                                                ExecutionNode* previous) {
  TRI_ASSERT(out != nullptr);

  bool const isDistinct = (expression->type == NODE_TYPE_DISTINCT);

  if (isDistinct) {
    TRI_ASSERT(expression->numMembers() == 1);
    expression = expression->getMember(0);
  }

  bool containsCollection = false;
  // replace occurrences of collection names used as function call arguments
  // (that are of type NODE_TYPE_COLLECTION) with their string equivalents
  // for example, this will turn `WITHIN(collection, ...)` into
  // `WITHIN("collection", ...)`
  auto visitor = [this, &containsCollection](AstNode* node) {
    if (node->type == NODE_TYPE_FCALL) {
      auto func = static_cast<Function*>(node->getData());

      // check function arguments
      auto args = node->getMember(0);
      size_t const n = args->numMembers();

      for (size_t i = 0; i < n; ++i) {
        auto member = args->getMemberUnchecked(i);
        auto conversion = func->getArgumentConversion(i);

        if (member->type == NODE_TYPE_COLLECTION &&
            (conversion == Function::Conversion::Required ||
             conversion == Function::Conversion::Optional)) {
          // collection attribute: no need to check for member simplicity
          args->changeMember(
              i, _ast->createNodeValueString(member->getStringValue(),
                                             member->getStringLength()));
        } else if (member->type == NODE_TYPE_VIEW) {
          // using views as function call parameters is not supported
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_NOT_IMPLEMENTED,
              "views cannot be used as arguments for function calls");
        }
      }
    } else if (node->type == NODE_TYPE_COLLECTION) {
      containsCollection = true;
    }

    return node;
  };

  // replace NODE_TYPE_COLLECTION function call arguments in the expression
  auto node = Ast::traverseAndModify(const_cast<AstNode*>(expression), visitor);

  // cppcheck-suppress knownConditionTrueFalse
  if (containsCollection) {
    // we found at least one occurence of NODE_TYPE_COLLECTION
    // now replace them with proper (FOR doc IN collection RETURN doc)
    // subqueries
    auto visitor = [this, &previous](AstNode* node) {
      if (node->type == NODE_TYPE_COLLECTION) {
        // collection name used inside an expression...

        auto& vocbase = _ast->query().vocbase();
        if (!vocbase.server()
                 .getFeature<QueryRegistryFeature>()
                 .allowCollectionsInExpressions()) {
          // this is disallowed here, so fail the query
          std::string cn = node->getString();
          THROW_ARANGO_EXCEPTION_PARAMS(
              TRI_ERROR_QUERY_COLLECTION_USED_IN_EXPRESSION, cn.c_str());
        }

        // create an on-the-fly subquery for a full collection access
        AstNode* rootNode = _ast->createNodeSubquery();

        // FOR part
        Variable* v = _ast->variables()->createTemporaryVariable();
        AstNode* forNode = _ast->createNodeFor(v, node, nullptr);
        // RETURN part
        AstNode* returnNode =
            _ast->createNodeReturn(_ast->createNodeReference(v));

        // add both nodes to subquery
        rootNode->addMember(forNode);
        rootNode->addMember(returnNode);

        // produce the proper ExecutionNodes from the subquery AST
        auto subquery = fromNode(rootNode);
        if (subquery == nullptr) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
        }

        // and register a reference to the subquery result in the expression
        v = _ast->variables()->createTemporaryVariable();
        auto en = createNode<SubqueryNode>(this, nextId(), subquery, v);
        _subqueries[v->id] = en;
        en->addDependency(previous);
        previous = en;
        return _ast->createNodeReference(v);
      }

      return node;
    };

    // replace remaining NODE_TYPE_COLLECTION occurrences in the expression
    node = Ast::traverseAndModify(node, visitor);
  }

  if (!isDistinct && node->type == NODE_TYPE_REFERENCE) {
    // further optimize if we are only left with a reference to a subquery
    auto subquery = getSubqueryFromExpression(node);

    if (subquery != nullptr) {
      // optimization: if the LET a = ... references a variable created by a
      // subquery,
      // change the output variable of the (anonymous) subquery to be the
      // outvariable of
      // the LET. and don't create the LET

      subquery->replaceOutVariable(out);
      return subquery;
    }
  }

  // generate a temporary calculation node
  auto expr = std::make_unique<Expression>(_ast, node);

  if (node->isConstant()) {
    AqlFunctionsInternalCache cache;
    FixedVarExpressionContext exprContext(_ast->query().trxForOptimization(),
                                          _ast->query(), cache);
    // We need to create a new AqlValue that copies the data here, otherwise
    // the data might be owned by the expression which might be destroyed too
    // soon, leaving us with an AqlValue with a dangling pointer!
    bool mustDestroy;  // can be ignored here; the variable takes ownership of
                       // the value.
    out->setConstantValue(
        AqlValue(expr->execute(&exprContext, mustDestroy).slice()));
  }

  CalculationNode* en =
      createNode<CalculationNode>(this, nextId(), std::move(expr), out);

  if (previous != nullptr) {
    en->addDependency(previous);
  }

  if (!isDistinct) {
    return en;
  }

  // DISTINCT expression is implemented by creating an anonymous COLLECT node
  auto collectNode = createAnonymousCollect(en);

  TRI_ASSERT(en != nullptr);
  collectNode->addDependency(en);

  return collectNode;
}

/// @brief get the subquery node from an expression
/// this will return a nullptr if the expression does not refer to a subquery
SubqueryNode* ExecutionPlan::getSubqueryFromExpression(
    AstNode const* expression) const {
  TRI_ASSERT(expression != nullptr);

  if (expression->type != NODE_TYPE_REFERENCE) {
    return nullptr;
  }

  auto referencedVariable = static_cast<Variable const*>(expression->getData());

  TRI_ASSERT(referencedVariable != nullptr);

  if (referencedVariable->isUserDefined()) {
    return nullptr;
  }

  auto it = _subqueries.find(referencedVariable->id);

  if (it == _subqueries.end()) {
    return nullptr;
  }

  return ExecutionNode::castTo<SubqueryNode*>((*it).second);
}

/// @brief get the output variable from a node
Variable const* ExecutionPlan::getOutVariable(ExecutionNode const* node) const {
  if (node->getType() == ExecutionNode::CALCULATION) {
    // CalculationNode has an outVariable() method
    return ExecutionNode::castTo<CalculationNode const*>(node)->outVariable();
  }

  if (node->getType() == ExecutionNode::COLLECT) {
    // CollectNode has an outVariable() method, but we cannot use it.
    // for CollectNode, outVariable() will return the variable filled by INTO,
    // but INTO is an optional feature
    // so this will return the first result variable of the COLLECT
    // this part of the code should only be called for anonymous COLLECT nodes,
    // which only have one result variable
    auto en = ExecutionNode::castTo<CollectNode const*>(node);
    auto const& vars = en->groupVariables();

    TRI_ASSERT(vars.size() == 1);
    auto v = vars[0].outVar;
    TRI_ASSERT(v != nullptr);
    return v;
  }

  if (node->getType() == ExecutionNode::SUBQUERY) {
    return ExecutionNode::castTo<SubqueryNode const*>(node)->outVariable();
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL,
      absl::StrCat("invalid node type '", node->getTypeString(),
                   "' in getOutVariable"));
}

/// @brief creates an anonymous COLLECT node (for a DISTINCT)
CollectNode* ExecutionPlan::createAnonymousCollect(
    CalculationNode const* previous) {
  // generate an out variable for the COLLECT
  auto out = _ast->variables()->createTemporaryVariable();
  TRI_ASSERT(out != nullptr);

  std::vector<GroupVarInfo> const groupVariables{
      GroupVarInfo{out, previous->outVariable()}};
  std::vector<AggregateVarInfo> const aggregateVariables{};

  CollectOptions options;
  options.fixMethod(CollectOptions::CollectMethod::kDistinct);

  auto en = createNode<CollectNode>(
      this, nextId(), std::move(options), std::move(groupVariables),
      std::move(aggregateVariables), nullptr, nullptr,
      std::vector<std::pair<Variable const*, std::string>>{},
      _ast->variables()->variables(false));

  return en;
}

bool ExecutionPlan::hasExclusiveAccessOption(AstNode const* node) {
  // parse the modification options we got
  if (node == nullptr || node->type != NODE_TYPE_OBJECT) {
    return false;
  }

  size_t n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMember(i);

    if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
      auto const name = member->getStringView();

      if (name == "exclusive") {
        auto value = member->getMember(0);
        TRI_ASSERT(value->isConstant());
        return value->isTrue();
      }
    }
  }

  return false;
}

/// @brief create modification options from an AST node
ModificationOptions ExecutionPlan::parseModificationOptions(
    QueryContext& query, std::string_view operationName, AstNode const* node,
    bool addWarnings) {
  TRI_ASSERT(operationName == "INSERT" || operationName == "UPDATE" ||
             operationName == "REPLACE" || operationName == "REMOVE" ||
             operationName == "UPSERT");

  ModificationOptions options;

  // parse the modification options we got
  if (node != nullptr && node->type == NODE_TYPE_OBJECT) {
    size_t n = node->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = node->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringView();
        auto value = member->getMember(0);

        TRI_ASSERT(value->isConstant());

        if (name == StaticStrings::WaitForSyncString) {
          options.waitForSync = value->isTrue();
        } else if (name == StaticStrings::SkipDocumentValidation) {
          options.validate = !value->isTrue();
        } else if (name == StaticStrings::KeepNullString) {
          options.keepNull = value->isTrue();
        } else if (name == StaticStrings::RefillIndexCachesString) {
          options.refillIndexCaches = value->isTrue()
                                          ? RefillIndexCaches::kRefill
                                          : RefillIndexCaches::kDontRefill;
        } else if (name == StaticStrings::MergeObjectsString) {
          options.mergeObjects = value->isTrue();
        } else if (name == StaticStrings::Overwrite) {
          // legacy: overwrite is set, superseded by overwriteMode
          // default behavior if only "overwrite" is specified
          if (!options.isOverwriteModeSet() && value->isTrue()) {
            options.overwriteMode = OperationOptions::OverwriteMode::Replace;
          }
        } else if (name == StaticStrings::OverwriteMode &&
                   value->isStringValue()) {
          auto overwriteMode =
              OperationOptions::determineOverwriteMode(value->getStringView());

          if (overwriteMode != OperationOptions::OverwriteMode::Unknown) {
            options.overwriteMode = overwriteMode;
          }
        } else if (name == StaticStrings::IgnoreRevsString) {
          options.ignoreRevs = value->isTrue();
        } else if (name == StaticStrings::VersionAttributeString &&
                   value->isStringValue() &&
                   (operationName == "INSERT" || operationName == "UPDATE" ||
                    operationName == "REPLACE")) {
          options.versionAttribute = value->getString();
        } else if (name == "exclusive") {
          options.exclusive = value->isTrue();
        } else if (name == "ignoreErrors") {
          options.ignoreErrors = value->isTrue();
        } else if (name == StaticStrings::ReadOwnWrites &&
                   operationName == "UPSERT") {
          // UPSERT supports "readOwnWrites" attribute, but other
          // modification options don't.
          // the value of readOwnWrites is already picked up during AST creation
          // and does not need to be handled here anymore.
        } else {
          if (addWarnings) {
            invalidOptionAttribute(query, "unknown", operationName, name);
          }
        }
      }
    }
  }

  return options;
}

/// @brief create modification options from an AST node
ModificationOptions ExecutionPlan::createModificationOptions(
    std::string_view operationName, AstNode const* node) {
  ModificationOptions options = parseModificationOptions(
      _ast->query(), operationName, node, /*addWarnings*/ true);
  return options;
}

/// @brief create COLLECT options from an AST node
CollectOptions ExecutionPlan::createCollectOptions(AstNode const* node) {
  CollectOptions options;

  // parse the modification options we got
  if (node != nullptr && node->type == NODE_TYPE_OBJECT) {
    size_t n = node->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = node->getMember(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        bool handled = false;
        auto const name = member->getStringView();
        if (name == "method") {
          auto value = member->getMember(0);
          if (value->isStringValue()) {
            options.method =
                CollectOptions::methodFromString(value->getStringView());
            if (options.method != CollectOptions::CollectMethod::kUndefined) {
              handled = true;
            }
          }
        } else if (name == "aggregateIntoExpressionOnDBServers") {
          auto value = member->getMember(0);
          if (value->isBoolValue()) {
            options.aggregateIntoExpressionOnDBServers = value->getBoolValue();
            handled = true;
          }
        }
        if (!handled) {
          invalidOptionAttribute(_ast->query(), "unknown", "COLLECT", name);
        }
      }
    }
  }

  return options;
}

/// @brief register a node with the plan
ExecutionNode* ExecutionPlan::registerNode(
    std::unique_ptr<ExecutionNode> node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->plan() == this);
  TRI_ASSERT(node->id() > ExecutionNodeId{0});
  TRI_ASSERT(!_ids.contains(node->id()));

  {
    ResourceUsageScope scope(
        _ast->query().resourceMonitor(),
        _trackMemoryUsage ? node->getMemoryUsedBytes() : 0);

    auto emplaced =
        _ids.try_emplace(node->id(), node.get()).second;  // take ownership
    TRI_ASSERT(emplaced);
    if (!emplaced) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unable to register node in plan");
    }

    // we are now responsible for tracking the memory usage
    scope.steal();
  }

  return node.release();
}

/// @brief register a node with the plan, will delete node if addition fails
ExecutionNode* ExecutionPlan::registerNode(ExecutionNode* node) {
  return registerNode(std::unique_ptr<ExecutionNode>(node));
}

SubqueryNode* ExecutionPlan::registerSubquery(SubqueryNode* node) {
  node = ExecutionNode::castTo<SubqueryNode*>(registerNode(node));
  _subqueries[node->outVariable()->id] = node;
  return node;
}

/// @brief creates a anonymous calculation node for an arbitrary expression
ExecutionNode* ExecutionPlan::createTemporaryCalculation(
    AstNode const* expression, ExecutionNode* previous) {
  // generate a temporary variable
  auto out = _ast->variables()->createTemporaryVariable();
  TRI_ASSERT(out != nullptr);

  return createCalculation(out, expression, previous);
}

/// @brief adds "previous" as dependency to "plan", returns "plan"
ExecutionNode* ExecutionPlan::addDependency(ExecutionNode* previous,
                                            ExecutionNode* plan) {
  TRI_ASSERT(previous != nullptr);
  TRI_ASSERT(plan != nullptr);

  try {
    plan->addDependency(previous);
    return plan;
  } catch (...) {
    // prevent memleak
    delete plan;
    throw;
  }
}

/// @brief create an execution plan element from an AST FOR node
ExecutionNode* ExecutionPlan::fromNodeFor(ExecutionNode* previous,
                                          AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_FOR);
  TRI_ASSERT(node->numMembers() == 3);

  auto variable = node->getMember(0);
  auto expression = node->getMember(1);
  auto options = node->getMember(2);

  // fetch 1st operand (out variable name)
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);

  ExecutionNode* en = nullptr;

  // peek at second operand
  if (expression->type == NODE_TYPE_COLLECTION) {
    // second operand is a collection
    std::string const collectionName = expression->getString();
    auto& collections = _ast->query().collections();
    auto collection = collections.get(collectionName);

    if (collection == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "no collection for EnumerateCollection");
    }
    IndexHint hint(_ast->query(), options,
                   IndexHint::FromCollectionOperation{});
    en = createNode<EnumerateCollectionNode>(this, nextId(), collection, v,
                                             false, std::move(hint));
    if (node->hasFlag(AstNodeFlagType::FLAG_READ_OWN_WRITES)) {
      // this is a FOR node that belongs to an UPSERT query
      ExecutionNode::castTo<EnumerateCollectionNode*>(en)->setCanReadOwnWrites(
          ReadOwnWrites::yes);
    }

    auto* dn = dynamic_cast<DocumentProducingNode*>(en);
    TRI_ASSERT(dn != nullptr);
    ::setForOptions(_ast->query(), options, dn);

  } else if (expression->type == NODE_TYPE_VIEW) {
    // second operand is a view
    std::string const viewName = expression->getString();
    auto& vocbase = _ast->query().vocbase();

    std::shared_ptr<LogicalView> view;

    if (ServerState::instance()->isSingleServer()) {
      view = vocbase.lookupView(viewName);
    } else {
      // need cluster wide view
      TRI_ASSERT(vocbase.server().hasFeature<ClusterFeature>());
      view =
          vocbase.server().getFeature<ClusterFeature>().clusterInfo().getView(
              vocbase.name(), viewName);
    }

    if (!view) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "no view for EnumerateView");
    }

    if (options->type == NODE_TYPE_NOP) {
      options = nullptr;
    }

    en = registerNode(new iresearch::IResearchViewNode(
        *this, nextId(), vocbase, view, *v, nullptr, options, {}));
  } else if (expression->type == NODE_TYPE_REFERENCE) {
    // second operand is already a variable
    auto inVariable = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(inVariable != nullptr);
    en = createNode<EnumerateListNode>(this, nextId(), inVariable, v);
  } else {
    // second operand is some misc. expression
    auto calc = createTemporaryCalculation(expression, previous);
    en = createNode<EnumerateListNode>(this, nextId(), getOutVariable(calc), v);
    previous = calc;
  }

  TRI_ASSERT(en != nullptr);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST FOR (view) node
ExecutionNode* ExecutionPlan::fromNodeForView(ExecutionNode* previous,
                                              AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_FOR_VIEW);
  TRI_ASSERT(node->numMembers() == 4);

  auto const* variable = node->getMember(0);
  TRI_ASSERT(variable);
  auto const* expression = node->getMember(1);
  TRI_ASSERT(expression);

  // fetch 1st operand (out variable name)
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v);

  ExecutionNode* en = nullptr;

  // peek at second operand
  if (expression->type != NODE_TYPE_VIEW) {
    // called for a non-view...
    if (expression->type == NODE_TYPE_COLLECTION) {
      std::string const name = expression->getString();

      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
          std::string(
              TRI_errno_string(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) +
              ": " + name);
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "SEARCH condition used on non-view");
    }
  }

  TRI_ASSERT(expression->type == NODE_TYPE_VIEW);

  std::string const viewName = expression->getString();
  auto& vocbase = _ast->query().vocbase();

  std::shared_ptr<LogicalView> view;

  if (ServerState::instance()->isSingleServer()) {
    view = vocbase.lookupView(viewName);
  } else {
    // need cluster wide view
    TRI_ASSERT(vocbase.server().hasFeature<ClusterFeature>());
    view = vocbase.server().getFeature<ClusterFeature>().clusterInfo().getView(
        vocbase.name(), viewName);
  }

  if (!view) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                   "no view for EnumerateView");
  }

  auto* search = node->getMember(2);
  TRI_ASSERT(search);
  TRI_ASSERT(search->type == NODE_TYPE_FILTER);
  TRI_ASSERT(search->numMembers() == 1);

  auto* options = node->getMemberUnchecked(3);
  TRI_ASSERT(options);

  if (options->type == NODE_TYPE_NOP) {
    options = nullptr;
  }

  en = registerNode(new iresearch::IResearchViewNode(
      *this, nextId(), vocbase, view, *v, search->getMember(0), options, {}));

  TRI_ASSERT(en != nullptr);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST FOR TRAVERSAL node
ExecutionNode* ExecutionPlan::fromNodeTraversal(ExecutionNode* previous,
                                                AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_TRAVERSAL);
  TRI_ASSERT(node->numMembers() >= 6);
  TRI_ASSERT(node->numMembers() <= 8);

  // the first 5 members are used by traversal internally.
  // The members 6-8, where 5 and 6 are optional, are used
  // as out variables.
  AstNode const* direction = node->getMember(0);
  AstNode const* start = node->getMember(1);
  AstNode const* graph = node->getMember(2);

  if (start->type == NODE_TYPE_OBJECT && start->isConstant()) {
    size_t n = start->numMembers();
    for (size_t i = 0; i < n; ++i) {
      auto member = start->getMember(i);
      if (member->type == NODE_TYPE_OBJECT_ELEMENT &&
          member->getStringView() == StaticStrings::IdString) {
        start = member->getMember(0);
        break;
      }
    }
  }

  if (start->type != NODE_TYPE_REFERENCE && start->type != NODE_TYPE_VALUE) {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(start, previous);
    start = _ast->createNodeReference(getOutVariable(calc));
    previous = calc;
  }

  // Prune Expression
  std::unique_ptr<Expression> pruneExpression =
      createPruneExpression(this, _ast, node->getMember(3));

  std::string errorReason;
  if (pruneExpression != nullptr &&
      !pruneExpression->canBeUsedInPrune(_ast->query().vocbase().isOneShard(),
                                         errorReason)) {
    // PRUNE is designed to be executed inside a DBServer. Therefore, we need a
    // check here and abort in cases which are just not allowed, e.g. execution
    // of user defined JavaScript method or V8 based methods.
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_PARSE,
        absl::StrCat("Invalid PRUNE expression: ", errorReason));
  }

  auto options =
      createTraversalOptions(getAst(), direction, node->getMember(4));

  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);
  direction = direction->getMember(0);
  TRI_ASSERT(direction->isIntValue());

  // First create the node
  auto travNode = createNode<TraversalNode>(
      this, nextId(), &(_ast->query().vocbase()), direction, start, graph,
      std::move(pruneExpression), std::move(options));

  auto variable = node->getMember(5);
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);
  travNode->setVertexOutput(v);

  if (node->numMembers() > 6) {
    // return the edge as well
    variable = node->getMember(6);
    TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
    v = static_cast<Variable*>(variable->getData());
    TRI_ASSERT(v != nullptr);
    travNode->setEdgeOutput(v);
    if (node->numMembers() > 7) {
      // return the path as well
      variable = node->getMember(7);
      TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
      v = static_cast<Variable*>(variable->getData());
      TRI_ASSERT(v != nullptr);
      travNode->setPathOutput(v);
    }
  }

  return addDependency(previous, travNode);
}

AstNode const* ExecutionPlan::parseTraversalVertexNode(ExecutionNode*& previous,
                                                       AstNode const* vertex) {
  if (vertex->type == NODE_TYPE_OBJECT && vertex->isConstant()) {
    size_t n = vertex->numMembers();
    for (size_t i = 0; i < n; ++i) {
      auto member = vertex->getMember(i);
      if (member->type == NODE_TYPE_OBJECT_ELEMENT &&
          member->getStringView() == StaticStrings::IdString) {
        vertex = member->getMember(0);
        break;
      }
    }
  }

  if (vertex->type != NODE_TYPE_REFERENCE && vertex->type != NODE_TYPE_VALUE) {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(vertex, previous);
    vertex = _ast->createNodeReference(getOutVariable(calc));
    // update previous so the caller has an updated value
    previous = calc;
  }

  return vertex;
}

/// @brief create an execution plan element from an AST for SHORTEST_PATH node
ExecutionNode* ExecutionPlan::fromNodeShortestPath(ExecutionNode* previous,
                                                   AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_SHORTEST_PATH);
  TRI_ASSERT(node->numMembers() >= 6);
  TRI_ASSERT(node->numMembers() <= 7);

  // the first 4 members are used by shortest_path internally.
  // The members 5-6, where 6 is optional, are used
  // as out variables.
  AstNode const* direction = node->getMember(0);
  AstNode const* start = parseTraversalVertexNode(previous, node->getMember(1));
  AstNode const* target =
      parseTraversalVertexNode(previous, node->getMember(2));
  AstNode const* graph = node->getMember(3);

  auto options =
      createPathsQueryOptions(getAst(), direction, node->getMember(4),
                              arangodb::graph::PathType::Type::ShortestPath);

  // First create the node
  auto spNode = createNode<ShortestPathNode>(
      this, nextId(), &(_ast->query().vocbase()), direction, start, target,
      graph, std::move(options));

  auto variable = node->getMember(5);
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);
  spNode->setVertexOutput(v);

  if (node->numMembers() > 6) {
    // return the edge as well
    variable = node->getMember(6);
    TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
    v = static_cast<Variable*>(variable->getData());
    TRI_ASSERT(v != nullptr);
    spNode->setEdgeOutput(v);
  }

  return addDependency(previous, spNode);
}

/// @brief create an execution plan element from an AST for ENUMERATE_PATHS node
ExecutionNode* ExecutionPlan::fromNodeEnumeratePaths(ExecutionNode* previous,
                                                     AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_ENUMERATE_PATHS);
  TRI_ASSERT(node->numMembers() == 7);

  auto const type = static_cast<arangodb::graph::PathType::Type>(
      node->getMember(0)->getIntValue());
  TRI_ASSERT(type == arangodb::graph::PathType::Type::KShortestPaths ||
             type == arangodb::graph::PathType::Type::KPaths ||
             type == arangodb::graph::PathType::Type::AllShortestPaths);

  // the first 5 members are used by shortest_path internally.
  // The members 6 is the out variable
  AstNode const* direction = node->getMember(1);
  AstNode const* start = parseTraversalVertexNode(previous, node->getMember(2));
  AstNode const* target =
      parseTraversalVertexNode(previous, node->getMember(3));
  AstNode const* graph = node->getMember(4);

  auto options =
      createPathsQueryOptions(getAst(), direction, node->getMember(5), type);

  // First create the node
  auto spNode = createNode<EnumeratePathsNode>(
      this, nextId(), &(_ast->query().vocbase()), type, direction, start,
      target, graph, std::move(options));

  auto variable = node->getMember(6);
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);
  spNode->setPathOutput(v);

  return addDependency(previous, spNode);
}

/// @brief create an execution plan element from an AST FILTER node
ExecutionNode* ExecutionPlan::fromNodeFilter(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_FILTER);
  TRI_ASSERT(node->numMembers() == 1);

  auto expression = node->getMember(0);

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(v != nullptr);
    en = createNode<FilterNode>(this, nextId(), v);
  } else {
    // operand is some misc expression
    if (expression->isTrue()) {
      // filter expression is known to be always true, so
      // remove the filter entirely
      return previous;
    }

    // note: if isTrue() is false above, it is not necessarily the case that
    // isFalse() is true next. The reason is that isTrue() and isFalse() only
    // return true in case of absolulete certainty. this requires expressions
    // to be based on query compile-time values only, but it will not work
    // for expressions that need to be evaluated at query runtime
    if (expression->isFalse()) {
      // filter expression is known to be always false, so
      // replace the FILTER with a NoResultsNode
      en = createNode<NoResultsNode>(this, nextId());
    } else {
      auto calc = createTemporaryCalculation(expression, previous);
      en = createNode<FilterNode>(this, nextId(), getOutVariable(calc));
      previous = calc;
    }
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST LET node
/// this also includes handling of subqueries (as subqueries can only occur
/// inside LET nodes)
ExecutionNode* ExecutionPlan::fromNodeLet(ExecutionNode* previous,
                                          AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_LET);
  TRI_ASSERT(node->numMembers() >= 2);

  AstNode const* variable = node->getMember(0);
  AstNode const* expression = node->getMember(1);

  auto v = static_cast<Variable*>(variable->getData());

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_SUBQUERY) {
    // operand is a subquery...
    ++_nestingLevel;
    auto subquery = fromNode(expression);
    --_nestingLevel;

    if (subquery == nullptr) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
    }

    en = createNode<SubqueryNode>(this, nextId(), subquery, v);
    _subqueries[ExecutionNode::castTo<SubqueryNode*>(en)->outVariable()->id] =
        en;
  } else {
    // check if the LET is a reference to a subquery
    auto subquery = getSubqueryFromExpression(expression);

    if (subquery != nullptr) {
      // optimization: if the LET a = ... references a variable created by a
      // subquery,
      // change the output variable of the (anonymous) subquery to be the
      // outvariable of
      // the LET. and don't create the LET

      subquery->replaceOutVariable(v);
      // We do not create a new node here, just return the last dependency.
      // Note that we *do not* want to return `subquery`, as we might leave a
      // dangling branch of ExecutionNodes from it.
      return previous;
    }
    // otherwise fall-through to normal behavior

    // operand is some misc expression, potentially including references to
    // other variables
    return createCalculation(v, expression, previous);
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST SORT node
ExecutionNode* ExecutionPlan::fromNodeSort(ExecutionNode* previous,
                                           AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_SORT);
  TRI_ASSERT(node->numMembers() == 1);

  auto list = node->getMember(0);
  TRI_ASSERT(list->type == NODE_TYPE_ARRAY);

  SortElementVector elements;

  size_t const n = list->numMembers();
  elements.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    auto element = list->getMember(i);
    TRI_ASSERT(element != nullptr);
    TRI_ASSERT(element->type == NODE_TYPE_SORT_ELEMENT);
    TRI_ASSERT(element->numMembers() == 2);

    auto expression = element->getMember(0);
    auto ascending = element->getMember(1);

    // get sort order
    bool isAscending;
    bool handled = false;
    if (ascending->type == NODE_TYPE_VALUE) {
      if (ascending->value.type == VALUE_TYPE_STRING) {
        // special treatment for string values ASC/DESC
        if (ascending->stringEqualsCaseInsensitive(
                StaticStrings::QuerySortASC)) {
          isAscending = true;
          handled = true;
        } else if (ascending->stringEqualsCaseInsensitive(
                       StaticStrings::QuerySortDESC)) {
          isAscending = false;
          handled = true;
        }
      }
    }

    if (!handled) {
      // if no sort order is set, ensure we have one
      AstNode const* ascendingNode = ascending->castToBool(_ast);
      if (ascendingNode->type == NODE_TYPE_VALUE &&
          ascendingNode->value.type == VALUE_TYPE_BOOL) {
        isAscending = ascendingNode->value.value._bool;
      } else {
        // must have an order
        isAscending = true;
      }
    }

    if (expression->type == NODE_TYPE_REFERENCE) {
      // sort operand is a variable
      auto v = static_cast<Variable*>(expression->getData());
      TRI_ASSERT(v != nullptr);
      elements.push_back(SortElement::create(v, isAscending));
    } else {
      // sort operand is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      elements.push_back(
          SortElement::create(getOutVariable(calc), isAscending));
      previous = calc;
    }
  }

  if (elements.empty()) {
    // no sort criterion remained - this can only happen if all sort
    // criteria were constants
    return previous;
  }

  // at least one sort criterion remained
  auto en = createNode<SortNode>(this, nextId(), elements, false);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST COLLECT node
ExecutionNode* ExecutionPlan::fromNodeCollect(ExecutionNode* previous,
                                              AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_COLLECT);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createCollectOptions(node->getMember(0));

  auto groups = node->getMember(1);
  size_t const numVars = groups->numMembers();

  std::vector<GroupVarInfo> groupVariables;
  groupVariables.reserve(numVars);

  for (size_t i = 0; i < numVars; ++i) {
    auto assigner = groups->getMember(i);

    if (assigner == nullptr) {
      continue;
    }

    TRI_ASSERT(assigner->type == NODE_TYPE_ASSIGN);
    auto out = assigner->getMember(0);
    TRI_ASSERT(out != nullptr);
    auto v = static_cast<Variable*>(out->getData());
    TRI_ASSERT(v != nullptr);

    auto expression = assigner->getMember(1);

    if (expression->type == NODE_TYPE_REFERENCE) {
      // operand is a variable
      auto e = static_cast<Variable*>(expression->getData());
      groupVariables.emplace_back(GroupVarInfo{v, e});
    } else {
      // operand is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      previous = calc;
      groupVariables.emplace_back(GroupVarInfo{v, getOutVariable(calc)});
    }
  }

  // aggregate variables
  AstNode* aggregates = node->getMember(2);
  std::vector<AggregateVarInfo> aggregateVars =
      prepareAggregateVars(&previous, aggregates);

  // handle out variable
  Variable const* outVariable = nullptr;
  auto const into = node->getMember(3);

  if (into->type != NODE_TYPE_NOP) {
    outVariable = static_cast<Variable const*>(into->getData());
  }

  Variable const* expressionVariable = nullptr;
  auto const expression = node->getMember(4);

  if (expression->type != NODE_TYPE_NOP) {
    if (expression->type == NODE_TYPE_REFERENCE) {
      // expression is already a variable
      auto variable = static_cast<Variable const*>(expression->getData());
      TRI_ASSERT(variable != nullptr);
      expressionVariable = variable;
    } else {
      // expression is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      previous = calc;
      expressionVariable = getOutVariable(calc);
    }
  }

  std::vector<std::pair<Variable const*, std::string>> keepVariables;
  auto const keep = node->getMember(5);

  if (keep->type != NODE_TYPE_NOP) {
    // variables to keep
    TRI_ASSERT(keep->type == NODE_TYPE_ARRAY);
    size_t const keepVarsSize = keep->numMembers();
    keepVariables.reserve(keepVarsSize);

    for (size_t i = 0; i < keepVarsSize; ++i) {
      auto ref = keep->getMember(i);
      TRI_ASSERT(ref->type == NODE_TYPE_REFERENCE);
      auto var = static_cast<Variable const*>(ref->getData());
      keepVariables.emplace_back(std::make_pair(var, var->name));
    }
  } else if (into->type != NODE_TYPE_NOP && expression->type == NODE_TYPE_NOP) {
    // `COLLECT ... INTO var ...` with neither an explicit expression
    // (INTO var = <expr>), nor an explicit `KEEP`.
    // In this case, we look for all previously defined user-defined variables
    // , which are in our scope, and which aren't top-level variables in the
    // (sub)query the COLLECT resides in.
    CollectNode::calculateAccessibleUserVariables(*previous, keepVariables);
  }

  auto en = createNode<CollectNode>(this, nextId(), options, groupVariables,
                                    aggregateVars, expressionVariable,
                                    outVariable, keepVariables,
                                    _ast->variables()->variables(false));

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST LIMIT node
ExecutionNode* ExecutionPlan::fromNodeLimit(ExecutionNode* previous,
                                            AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_LIMIT);
  TRI_ASSERT(node->numMembers() == 2);

  auto offset = node->getMember(0);
  auto count = node->getMember(1);

  if (offset->type != NODE_TYPE_VALUE || count->type != NODE_TYPE_VALUE) {
    if (_ast->query().queryOptions().usePlanCache) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_NOT_ELIGIBLE_FOR_PLAN_CACHING);
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
          "LIMIT offset/count values must be constant numeric values");
    }
  }

  TRI_ASSERT(offset->type == NODE_TYPE_VALUE);
  TRI_ASSERT(count->type == NODE_TYPE_VALUE);

  int64_t offsetValue = 0;
  if (offset->value.type != VALUE_TYPE_NULL) {
    if ((offset->value.type != VALUE_TYPE_INT &&
         offset->value.type != VALUE_TYPE_DOUBLE) ||
        offset->getIntValue() < 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
          "LIMIT offset value is not a number or out of range");
    }
    offsetValue = offset->getIntValue();
  }

  int64_t countValue =
      128 * 1024 *
      1024;  // arbitrary default value for an "unbounded" limit value
  if (count->value.type != VALUE_TYPE_NULL) {
    if ((count->value.type != VALUE_TYPE_INT &&
         count->value.type != VALUE_TYPE_DOUBLE) ||
        count->getIntValue() < 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
          "LIMIT count value is not a number or out of range");
    }
    countValue = count->getIntValue();
  }

  auto en =
      createNode<LimitNode>(this, nextId(), static_cast<size_t>(offsetValue),
                            static_cast<size_t>(countValue));

  if (_nestingLevel == 0) {
    _lastLimitNode = en;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST RETURN node
ExecutionNode* ExecutionPlan::fromNodeReturn(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_RETURN);
  TRI_ASSERT(node->numMembers() == 1);

  auto expression = node->getMember(0);

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(v != nullptr);
    en = createNode<ReturnNode>(this, nextId(), v);
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);
    en = createNode<ReturnNode>(this, nextId(), getOutVariable(calc));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST REMOVE node
ExecutionNode* ExecutionPlan::fromNodeRemove(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_REMOVE);
  TRI_ASSERT(node->numMembers() == 4);

  auto options = createModificationOptions("REMOVE", node->getMember(0));
  std::string const collectionName = node->getMember(1)->getString();
  auto const& collections = _ast->query().collections();
  auto* collection = collections.get(collectionName);

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no collection for RemoveNode");
  }
  if (options.exclusive) {
    collection->setExclusiveAccess();
  }

  auto expression = node->getMember(2);
  ExecutionNode* en = nullptr;
  auto returnVarNode = node->getMember(3);
  Variable const* outVariableOld =
      static_cast<Variable*>(returnVarNode->getData());

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());

    TRI_ASSERT(v != nullptr);
    en = createNode<RemoveNode>(this, nextId(), collection, options, v,
                                outVariableOld);
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);

    en = createNode<RemoveNode>(this, nextId(), collection, options,
                                getOutVariable(calc), outVariableOld);
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST INSERT node
ExecutionNode* ExecutionPlan::fromNodeInsert(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_INSERT);
  TRI_ASSERT(node->numMembers() > 3);
  TRI_ASSERT(node->numMembers() < 6);

  auto options = createModificationOptions("INSERT", node->getMember(0));
  std::string const collectionName = node->getMember(1)->getString();
  auto const& collections = _ast->query().collections();
  auto* collection = collections.get(collectionName);

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no collection for InsertNode");
  }
  if (options.exclusive) {
    collection->setExclusiveAccess();
  }

  auto expression = node->getMember(2);
  auto returnVarNode = node->getMember(3);
  Variable const* outVariableNew =
      static_cast<Variable*>(returnVarNode->getData());

  Variable const* outVariableOld = nullptr;
  if (node->numMembers() == 5) {
    returnVarNode = node->getMember(4);
    outVariableOld = static_cast<Variable*>(returnVarNode->getData());
  }

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());

    TRI_ASSERT(v != nullptr);
    en = createNode<InsertNode>(this, nextId(), collection, options, v,
                                outVariableOld, outVariableNew);
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);

    en = createNode<InsertNode>(this, nextId(), collection, options,
                                getOutVariable(calc), outVariableOld,
                                outVariableNew);
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST UPDATE node
ExecutionNode* ExecutionPlan::fromNodeUpdate(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_UPDATE);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createModificationOptions("UPDATE", node->getMember(0));
  std::string const collectionName = node->getMember(1)->getString();
  auto const& collections = _ast->query().collections();
  auto* collection = collections.get(collectionName);

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no collection for UpdateNode");
  }
  if (options.exclusive) {
    collection->setExclusiveAccess();
  }

  auto docExpression = node->getMember(2);
  auto keyExpression = node->getMember(3);
  Variable const* keyVariable = nullptr;
  ExecutionNode* en = nullptr;

  Variable const* outVariableOld =
      static_cast<Variable*>(node->getMember(4)->getData());
  Variable const* outVariableNew =
      static_cast<Variable*>(node->getMember(5)->getData());

  if (keyExpression->type == NODE_TYPE_NOP) {
    keyExpression = nullptr;
  }

  if (keyExpression != nullptr) {
    if (keyExpression->type == NODE_TYPE_REFERENCE) {
      // key operand is already a variable
      auto v = static_cast<Variable*>(keyExpression->getData());
      TRI_ASSERT(v != nullptr);
      keyVariable = v;
    } else {
      // key operand is some misc expression
      auto calc = createTemporaryCalculation(keyExpression, previous);
      previous = calc;
      keyVariable = getOutVariable(calc);
    }
  }

  if (docExpression->type == NODE_TYPE_REFERENCE) {
    // document operand is already a variable
    auto v = static_cast<Variable*>(docExpression->getData());

    TRI_ASSERT(v != nullptr);
    en = createNode<UpdateNode>(this, nextId(), collection, options, v,
                                keyVariable, outVariableOld, outVariableNew);
  } else {
    // document operand is some misc expression
    auto calc = createTemporaryCalculation(docExpression, previous);

    en = createNode<UpdateNode>(this, nextId(), collection, options,
                                getOutVariable(calc), keyVariable,
                                outVariableOld, outVariableNew);
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST REPLACE node
ExecutionNode* ExecutionPlan::fromNodeReplace(ExecutionNode* previous,
                                              AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_REPLACE);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createModificationOptions("REPLACE", node->getMember(0));
  std::string const collectionName = node->getMember(1)->getString();
  auto const& collections = _ast->query().collections();
  auto* collection = collections.get(collectionName);

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no collection for ReplaceNode");
  }
  if (options.exclusive) {
    collection->setExclusiveAccess();
  }

  auto docExpression = node->getMember(2);
  auto keyExpression = node->getMember(3);
  Variable const* keyVariable = nullptr;
  ExecutionNode* en = nullptr;

  Variable const* outVariableOld =
      static_cast<Variable*>(node->getMember(4)->getData());
  Variable const* outVariableNew =
      static_cast<Variable*>(node->getMember(5)->getData());

  if (keyExpression->type == NODE_TYPE_NOP) {
    keyExpression = nullptr;
  }

  if (keyExpression != nullptr) {
    if (keyExpression->type == NODE_TYPE_REFERENCE) {
      // key operand is already a variable
      auto v = static_cast<Variable*>(keyExpression->getData());
      TRI_ASSERT(v != nullptr);
      keyVariable = v;
    } else {
      // key operand is some misc expression
      auto calc = createTemporaryCalculation(keyExpression, previous);
      previous = calc;
      keyVariable = getOutVariable(calc);
    }
  }

  if (docExpression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(docExpression->getData());

    TRI_ASSERT(v != nullptr);
    en = createNode<ReplaceNode>(this, nextId(), collection, options, v,
                                 keyVariable, outVariableOld, outVariableNew);
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(docExpression, previous);

    en = createNode<ReplaceNode>(this, nextId(), collection, options,
                                 getOutVariable(calc), keyVariable,
                                 outVariableOld, outVariableNew);
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST UPSERT node
ExecutionNode* ExecutionPlan::fromNodeUpsert(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_UPSERT);
  TRI_ASSERT(node->numMembers() == 7);

  auto options = createModificationOptions("UPSERT", node->getMember(0));
  std::string const collectionName = node->getMember(1)->getString();
  auto const& collections = _ast->query().collections();
  auto* collection = collections.get(collectionName);

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "no collection for UpsertNode");
  }
  if (options.exclusive) {
    collection->setExclusiveAccess();
  }

  if (node->hasFlag(AstNodeFlagType::FLAG_READ_OWN_WRITES) &&
      ServerState::instance()->isCoordinator()) {
    // we are in the cluster, and the UPSERT node has the
    // "readOwnWrites" option not set or set to true.
    if (collection->numberOfShards() > 1) {
      // collection with multiple shards. the query will use
      // a DistributeNode, and the UPSERT cannot guarantee that
      // its lookup part has observed all previous writes.
      _ast->query().warnings().registerWarning(
          TRI_ERROR_QUERY_INVALID_OPTIONS_ATTRIBUTE,
          absl::StrCat("UPSERT operation on sharded collection '",
                       collection->name(),
                       "' cannot guarantee that own writes can be fully "
                       "observed. To opt out of this warning, set the "
                       "`readOwnWrites` option to `false` for the UPSERT"));
    } else {
#ifdef USE_ENTERPRISE
      bool warnAboutSingleShardCollections = false;
#else
      bool warnAboutSingleShardCollections =
          !_ast->query().vocbase().isOneShard();
#endif
      if (warnAboutSingleShardCollections) {
        // single-shard collection. if we are not in a OneShard
        // database, the query will also use a DistributeNode, and
        // the UPSERT cannot guarantee that its lookup part has
        // observed all previous writes.
        _ast->query().warnings().registerWarning(
            TRI_ERROR_QUERY_INVALID_OPTIONS_ATTRIBUTE,
            absl::StrCat("UPSERT operation on collection '", collection->name(),
                         "' cannot guarantee that own writes can be fully "
                         "observed. To opt out of this warning, set the "
                         "`readOwnWrites` option to `false` for the UPSERT"));
      }
    }
  }

  auto docExpression = node->getMember(2);
  auto insertExpression = node->getMember(3);
  auto updateExpression = node->getMember(4);
  Variable const* outVariableNew =
      static_cast<Variable*>(node->getMember(6)->getData());

  TRI_ASSERT(docExpression->type == NODE_TYPE_REFERENCE);
  // doc operand is already a variable
  auto docVariable = static_cast<Variable*>(docExpression->getData());
  TRI_ASSERT(docVariable != nullptr);

  Variable const* insertVar;
  if (insertExpression->type == NODE_TYPE_REFERENCE) {
    // insert operand is already a variable
    insertVar = static_cast<Variable*>(insertExpression->getData());
  } else {
    auto calc = createTemporaryCalculation(insertExpression, previous);
    previous = calc;
    insertVar = getOutVariable(calc);
  }
  TRI_ASSERT(insertVar != nullptr);

  Variable const* updateVar;
  if (updateExpression->type == NODE_TYPE_REFERENCE) {
    // insert operand is already a variable
    updateVar = static_cast<Variable*>(updateExpression->getData());
  } else {
    auto calc = createTemporaryCalculation(updateExpression, previous);
    previous = calc;
    updateVar = getOutVariable(calc);
  }
  TRI_ASSERT(updateVar != nullptr);

  bool isReplace =
      (node->getIntValue(true) == static_cast<int64_t>(NODE_TYPE_REPLACE));
  bool canReadOwnWrites = node->hasFlag(AstNodeFlagType::FLAG_READ_OWN_WRITES);
  ExecutionNode* en = createNode<UpsertNode>(
      this, nextId(), collection, options, docVariable, insertVar, updateVar,
      outVariableNew, isReplace, canReadOwnWrites);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST WINDOW node
ExecutionNode* ExecutionPlan::fromNodeWindow(ExecutionNode* previous,
                                             AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_WINDOW);
  TRI_ASSERT(node->numMembers() == 3);

  auto spec = node->getMember(0);
  auto rangeExpr = node->getMember(1);
  auto aggregates = node->getMember(2);

  AqlFunctionsInternalCache cache;
  FixedVarExpressionContext exprContext(_ast->query().trxForOptimization(),
                                        _ast->query(), cache);

  Variable const* rangeVar = nullptr;
  if (rangeExpr->type != NODE_TYPE_NOP) {
    if (rangeExpr->type == NODE_TYPE_REFERENCE) {
      // operand is a variable
      rangeVar = static_cast<Variable*>(rangeExpr->getData());
    } else {  // need to add a calculation
      auto calc = createTemporaryCalculation(rangeExpr, previous);
      previous = calc;
      rangeVar = getOutVariable(calc);
    }

    // add a sort on rangeVariable in front of the WINDOW
    SortElementVector elements;
    elements.push_back(SortElement::create(rangeVar, /*isAscending*/ true));
    auto en = registerNode(
        std::make_unique<SortNode>(this, nextId(), std::move(elements), false));
    previous = addDependency(previous, en);
  }

  AqlValue preceding, following;
  TRI_ASSERT(spec->type == NODE_TYPE_OBJECT);

  size_t n = spec->numMembers();
  for (size_t i = 0; i < n; ++i) {
    auto member = spec->getMemberUnchecked(i);

    if (member == nullptr || member->type != NODE_TYPE_OBJECT_ELEMENT) {
      continue;
    }
    auto const name = member->getStringView();
    AstNode* value = member->getMember(0);
    if (!value->isConstant()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_COMPILE_TIME_OPTIONS,
          "WINDOW bounds must be determined at compile time");
    }

    bool mustDestroy = false;
    if (name == "preceding") {
      Expression expr(_ast, value);
      AqlValue val = expr.execute(&exprContext, mustDestroy);
      if (!mustDestroy && val.isPointer()) {  // force a copy
        preceding = AqlValue(val.slice());
      } else {
        preceding = val.clone();
      }
    } else if (name == "following") {
      Expression expr(_ast, value);
      AqlValue val = expr.execute(&exprContext, mustDestroy);
      if (!mustDestroy && val.isPointer()) {  // force a copy
        following = AqlValue(val.slice());
      } else {
        following = val.clone();
      }
    }
  }

  auto type =
      rangeVar != nullptr ? WindowBounds::Type::Range : WindowBounds::Type::Row;

  // aggregate variables
  std::vector<AggregateVarInfo> aggregateVariables =
      prepareAggregateVars(&previous, aggregates);
  auto en = registerNode(std::make_unique<WindowNode>(
      this, nextId(),
      WindowBounds(type, std::move(preceding), std::move(following)), rangeVar,
      aggregateVariables));
  return addDependency(previous, en);
}

/// @brief create an execution plan from an abstract syntax tree node
ExecutionNode* ExecutionPlan::fromNode(AstNode const* node) {
  TRI_ASSERT(node != nullptr);

  ExecutionNode* en =
      createNode<SingletonNode>(this, nextId(), _ast->bindParameterVariables());

  size_t const n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member == nullptr || member->type == NODE_TYPE_NOP) {
      continue;
    }

    switch (member->type) {
      case NODE_TYPE_WITH: {
        // the using declaration...
        break;
      }

      case NODE_TYPE_FOR: {
        en = fromNodeFor(en, member);
        break;
      }

      case NODE_TYPE_FOR_VIEW: {
        en = fromNodeForView(en, member);
        break;
      }

      case NODE_TYPE_TRAVERSAL: {
        en = fromNodeTraversal(en, member);
        break;
      }

      case NODE_TYPE_SHORTEST_PATH: {
        en = fromNodeShortestPath(en, member);
        break;
      }

      case NODE_TYPE_ENUMERATE_PATHS: {
        en = fromNodeEnumeratePaths(en, member);
        break;
      }
      case NODE_TYPE_FILTER: {
        en = fromNodeFilter(en, member);
        break;
      }

      case NODE_TYPE_LET: {
        en = fromNodeLet(en, member);
        break;
      }

      case NODE_TYPE_SORT: {
        en = fromNodeSort(en, member);
        break;
      }

      case NODE_TYPE_COLLECT: {
        en = fromNodeCollect(en, member);
        break;
      }

      case NODE_TYPE_LIMIT: {
        en = fromNodeLimit(en, member);
        break;
      }

      case NODE_TYPE_RETURN: {
        en = fromNodeReturn(en, member);
        break;
      }

      case NODE_TYPE_REMOVE: {
        en = fromNodeRemove(en, member);
        break;
      }

      case NODE_TYPE_INSERT: {
        en = fromNodeInsert(en, member);
        break;
      }

      case NODE_TYPE_UPDATE: {
        en = fromNodeUpdate(en, member);
        break;
      }

      case NODE_TYPE_REPLACE: {
        en = fromNodeReplace(en, member);
        break;
      }

      case NODE_TYPE_UPSERT: {
        en = fromNodeUpsert(en, member);
        break;
      }

      case NODE_TYPE_WINDOW: {
        en = fromNodeWindow(en, member);
        break;
      }

      default: {
        // node type not implemented
        en = nullptr;
        break;
      }
    }

    TRI_ASSERT(en != nullptr);

    if (en == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          absl::StrCat("type not handled: ", member->getTypeString()));
    }

    if (_ids.size() > maxPlanNodes) {
      // maximum number of execution nodes reached
      // we have to limit this to prevent super-long runtimes for query
      // optimization and execution
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_TOO_MUCH_NESTING,
          absl::StrCat("total number of generated execution nodes for query "
                       "execution plans reached the maximum possible value (",
                       maxPlanNodes, ")"));
    }
  }

  return en;
}

template<WalkerUniqueness U>
/// @brief find nodes of certain types
void ExecutionPlan::findNodesOfType(
    containers::SmallVector<ExecutionNode*, 8>& result,
    std::initializer_list<ExecutionNode::NodeType> const& types,
    bool enterSubqueries) const {
  // check if any of the node types is actually present in the plan
  bool haveNodes = std::any_of(
      types.begin(), types.end(),
      [this](ExecutionNode::NodeType type) -> bool { return contains(type); });
  if (haveNodes) {
    // found a node type that is in the plan
    NodeFinder<std::initializer_list<ExecutionNode::NodeType>, U> finder(
        types, result, enterSubqueries);
    root()->walk(finder);
  }
}

/// @brief find nodes of a certain type
void ExecutionPlan::findNodesOfType(
    containers::SmallVector<ExecutionNode*, 8>& result,
    ExecutionNode::NodeType type, bool enterSubqueries) const {
  findNodesOfType<WalkerUniqueness::NonUnique>(result, {type}, enterSubqueries);
}

/// @brief find nodes of certain types
void ExecutionPlan::findNodesOfType(
    containers::SmallVector<ExecutionNode*, 8>& result,
    std::initializer_list<ExecutionNode::NodeType> const& types,
    bool enterSubqueries) const {
  findNodesOfType<WalkerUniqueness::NonUnique>(result, types, enterSubqueries);
}

/// @brief find nodes of certain types
void ExecutionPlan::findUniqueNodesOfType(
    containers::SmallVector<ExecutionNode*, 8>& result,
    std::initializer_list<ExecutionNode::NodeType> const& types,
    bool enterSubqueries) const {
  findNodesOfType<WalkerUniqueness::Unique>(result, types, enterSubqueries);
}

/// @brief find all end nodes in a plan
void ExecutionPlan::findEndNodes(
    containers::SmallVector<ExecutionNode*, 8>& result,
    bool enterSubqueries) const {
  EndNodeFinder finder(result, enterSubqueries);
  root()->walk(finder);
}

/// @brief determine and set _varsUsedLater in all nodes
/// as a side effect, count the different types of nodes in the plan
/// and if there are any forced index hints left in the plan on collection
/// nodes
void ExecutionPlan::findVarUsage() {
  if (varUsageComputed()) {
    return;
  }

  // reset all counters
  for (auto& counter : _typeCounts) {
    counter = 0;
  }
  _hasForcedIndexHints = false;
  _varSetBy.clear();
  ::VarUsageFinder finder(&_varSetBy);
  root()->walk(finder);

  setVarUsageComputed();
}

/// @brief determine if the above are already set
bool ExecutionPlan::varUsageComputed() const { return _varUsageComputed; }

void ExecutionPlan::planRegisters(ExplainRegisterPlan explainRegisterPlan) {
  _root->planRegisters(explainRegisterPlan);
}

/// @brief unlinkNodes, note that this does not delete the removed
/// nodes and that one cannot remove the root node of the plan.
void ExecutionPlan::unlinkNodes(
    std::unordered_set<ExecutionNode*> const& toRemove) {
  for (auto& node : toRemove) {
    unlinkNode(node);
  }
}

void ExecutionPlan::unlinkNodes(
    ::arangodb::containers::HashSet<ExecutionNode*> const& toRemove) {
  for (auto& node : toRemove) {
    unlinkNode(node);
  }
}

/// @brief unlinkNode, note that this does not delete the removed
/// node and that one cannot remove the root node of the plan.
void ExecutionPlan::unlinkNode(ExecutionNode* node, bool allowUnlinkingRoot) {
  auto parents = node->getParents();

  if (parents.empty()) {
    if (!allowUnlinkingRoot) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "Cannot unlink root node of plan");
    }
    // adjust root node. the caller needs to make sure that a new root node
    // gets inserted
    if (node == _root) {
      _root = nullptr;
    }
  }

  auto dep = node->getDependencies();  // Intentionally copy the vector!
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  for (auto const& it : dep) {
    TRI_ASSERT(it != nullptr);
  }
#endif

  for (auto* p : parents) {
    p->removeDependency(node);

    for (auto* x : dep) {
      TRI_ASSERT(x != nullptr);
      p->addDependency(x);
    }
  }

  for (auto* x : dep) {
    TRI_ASSERT(x != nullptr);
    node->removeDependency(x);
  }
  clearVarUsageComputed();
}

/// @brief replaceNode, note that <newNode> must be registered with the plan
/// before this method is called, also this does not delete the old
/// node and that one cannot replace the root node of the plan.
void ExecutionPlan::replaceNode(ExecutionNode* oldNode,
                                ExecutionNode* newNode) {
  TRI_ASSERT(oldNode->id() != newNode->id());
  TRI_ASSERT(newNode->getDependencies().empty());
  TRI_ASSERT(oldNode != _root);
  TRI_ASSERT(newNode != nullptr);

  // Intentional copy
  std::vector<ExecutionNode*> deps = oldNode->getDependencies();

  for (auto* x : deps) {
    TRI_ASSERT(x != nullptr);
    newNode->addDependency(x);
    oldNode->removeDependency(x);
  }

  // Intentional copy
  auto oldNodeParents = oldNode->getParents();

  for (auto* oldNodeParent : oldNodeParents) {
    if (!oldNodeParent->replaceDependency(oldNode, newNode)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "Could not replace dependencies of an old node");
    }
  }
  clearVarUsageComputed();
}

/// @brief insert <newNode> as a new (the first!) dependency of
/// <oldNode> and make the former first dependency of <oldNode> a
/// dependency of <newNode> (and no longer a direct dependency of
/// <oldNode>).
/// <newNode> must be registered with the plan before this method is called.
void ExecutionPlan::insertDependency(ExecutionNode* oldNode,
                                     ExecutionNode* newNode) {
  TRI_ASSERT(newNode != nullptr);
  TRI_ASSERT(oldNode->id() != newNode->id());
  TRI_ASSERT(newNode->getDependencies().empty());
  TRI_ASSERT(oldNode->getDependencies().size() == 1);

  auto oldDeps = oldNode->getDependencies();  // Intentional copy
  TRI_ASSERT(!oldDeps.empty());

  if (!oldNode->replaceDependency(oldDeps[0], newNode)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "Could not replace dependencies of an old node");
  }

  newNode->removeDependencies();
  TRI_ASSERT(oldDeps[0] != nullptr);
  newNode->addDependency(oldDeps[0]);
  clearVarUsageComputed();
}

/// @brief insert note directly after previous
/// will remove previous as a dependency from its parents and
/// add newNode as a dependency. <newNode> must be registered with the plan
void ExecutionPlan::insertAfter(ExecutionNode* previous,
                                ExecutionNode* newNode) {
  TRI_ASSERT(newNode != nullptr);
  TRI_ASSERT(previous->id() != newNode->id());
  TRI_ASSERT(newNode->getDependencies().empty());

  std::vector<ExecutionNode*> parents =
      previous->getParents();  // Intentional copy
  for (ExecutionNode* parent : parents) {
    if (!parent->replaceDependency(previous, newNode)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "Could not replace dependencies of an old node");
    }
  }
  newNode->addDependency(previous);
}

/// @brief insert node directly before current
void ExecutionPlan::insertBefore(ExecutionNode* current,
                                 ExecutionNode* newNode) {
  TRI_ASSERT(newNode != nullptr);
  TRI_ASSERT(current->id() != newNode->id());
  TRI_ASSERT(newNode->getDependencies().empty());
  TRI_ASSERT(!newNode->hasParent());

  for (auto* dep : current->getDependencies()) {
    newNode->addDependency(dep);
  }
  current->removeDependencies();
  current->addDependency(newNode);
}

/// @brief create a plan from VPack
ExecutionNode* ExecutionPlan::fromSlice(velocypack::Slice slice,
                                        bool simpleSnippetFormat) {
  VPackSlice nodes = VPackSlice::noneSlice();

  if (simpleSnippetFormat) {
    // simple format. we are expecting just an array with the nodes
    nodes = slice;
  } else {
    // complex format. we are expecting an object with a "nodes" attribute
    // that contains an array with the nodes
    if (!slice.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "plan slice is not an object");
    }

    nodes = slice.get("nodes");
  }

  if (!nodes.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "plan \"nodes\" attribute is not an array");
  }

  ExecutionNode* ret = nullptr;

  // first, re-create all nodes from the Slice, using the node ids
  // no dependency links will be set up in this step
  for (VPackSlice it : VPackArrayIterator(nodes)) {
    if (!it.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "node entry in plan is not an object");
    }

    ret = ExecutionNode::fromVPackFactory(this, it);
    // We need to adjust nextId here, otherwise we cannot add new nodes to
    // this plan anymore
    if (_nextId <= ret->id()) {
      _nextId = ExecutionNodeId{ret->id().id() + 1};
    }
    registerNode(ret);

    // we have to count all nodes by their type here, because our caller
    // will set the _varUsageComputed flag to true manually, bypassing the
    // regular counting!
    increaseCounter(*ret);

    TRI_ASSERT(ret != nullptr);

    if (ret->getType() == arangodb::aql::ExecutionNode::SUBQUERY) {
      // found a subquery node. now do magick here
      VPackSlice subquery = it.get("subquery");
      // create the subquery nodes from the "subquery" sub-node
      auto subqueryNode = fromSlice(subquery, /*simple*/ false);

      // register the just created subquery
      ExecutionNode::castTo<SubqueryNode*>(ret)->setSubquery(subqueryNode,
                                                             false);
    }
  }

  // all nodes have been created. now add the dependencies
  for (VPackSlice it : VPackArrayIterator(nodes)) {
    // read the node's own id
    auto thisId = ExecutionNodeId{
        it.get("id").getNumericValue<ExecutionNodeId::BaseType>()};
    auto thisNode = getNodeById(thisId);

    // now re-link the dependencies
    VPackSlice dependencies = it.get("dependencies");
    if (dependencies.isArray()) {
      for (VPackSlice const it2 : VPackArrayIterator(dependencies)) {
        if (it2.isNumber()) {
          auto depId =
              ExecutionNodeId{it2.getNumericValue<ExecutionNodeId::BaseType>()};
          thisNode->addDependency(getNodeById(depId));
        }
      }
    }
  }

  return ret;
}

bool ExecutionPlan::fullCount() const noexcept {
  LimitNode* lastLimitNode =
      _lastLimitNode == nullptr
          ? nullptr
          : ExecutionNode::castTo<LimitNode*>(_lastLimitNode);
  return lastLimitNode != nullptr && lastLimitNode->fullCount();
}

/// @brief find all variables that are populated with data from collections
void ExecutionPlan::findCollectionAccessVariables() {
  _ast->variables()->visit([this](Variable* variable) {
    ExecutionNode* node = this->getVarSetBy(variable->id);
    if (node == nullptr) {
      return;
    }
    if (node->getType() == ExecutionNode::ENUMERATE_COLLECTION ||
        node->getType() == ExecutionNode::INDEX) {
      // we only produce full documents if there are no projections
      variable->isFullDocumentFromCollection =
          ExecutionNode::castTo<DocumentProducingNode const*>(node)
              ->projections()
              .empty();
    } else if (node->getType() == ExecutionNode::INSERT ||
               node->getType() == ExecutionNode::UPDATE ||
               node->getType() == ExecutionNode::REPLACE ||
               node->getType() == ExecutionNode::REMOVE) {
      // these nodes always return full documents
      variable->isFullDocumentFromCollection = true;
    } else if (node->getType() == ExecutionNode::ENUMERATE_IRESEARCH_VIEW) {
      // views always return full documents
      variable->isFullDocumentFromCollection = true;
    } else if (node->getType() == ExecutionNode::MATERIALIZE) {
      // materialize nodes always return full documents
      variable->isFullDocumentFromCollection = true;
    } else if (node->getType() == ExecutionNode::TRAVERSAL) {
      TraversalNode const* tn =
          ExecutionNode::castTo<TraversalNode const*>(node);
      if (variable == tn->vertexOutVariable() ||
          variable == tn->edgeOutVariable()) {
        // traversal vertices and edges are always returned as full documents
        variable->isFullDocumentFromCollection = true;
      }
    }
    // TODO: maybe add more node types here?
  });
}

void ExecutionPlan::prepareTraversalOptions() {
  containers::SmallVector<ExecutionNode*, 8> nodes;
  findNodesOfType(nodes,
                  {arangodb::aql::ExecutionNode::TRAVERSAL,
                   arangodb::aql::ExecutionNode::SHORTEST_PATH,
                   arangodb::aql::ExecutionNode::ENUMERATE_PATHS},
                  true);
  for (auto& node : nodes) {
    switch (node->getType()) {
      case ExecutionNode::TRAVERSAL:
      case ExecutionNode::SHORTEST_PATH:
      case ExecutionNode::ENUMERATE_PATHS: {
        auto* graphNode = ExecutionNode::castTo<GraphNode*>(node);
        graphNode->prepareOptions();
      } break;
      default:
        TRI_ASSERT(false);
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    }
  }
}

std::vector<AggregateVarInfo> ExecutionPlan::prepareAggregateVars(
    ExecutionNode** previous, AstNode const* node) {
  std::vector<AggregateVarInfo> aggregateVariables;

  TRI_ASSERT(node->type == NODE_TYPE_AGGREGATIONS);
  AstNode* list = node->getMember(0);
  TRI_ASSERT(list->type == NODE_TYPE_ARRAY);
  size_t const numVars = list->numMembers();

  aggregateVariables.reserve(numVars);
  for (size_t i = 0; i < numVars; ++i) {
    auto assigner = list->getMemberUnchecked(i);

    if (assigner == nullptr) {
      continue;
    }

    TRI_ASSERT(assigner->type == NODE_TYPE_ASSIGN);
    auto out = assigner->getMember(0);
    TRI_ASSERT(out != nullptr);
    auto outVar = static_cast<Variable*>(out->getData());
    TRI_ASSERT(outVar != nullptr);

    auto expression = assigner->getMember(1);

    // operand is always a function call
    TRI_ASSERT(expression->type == NODE_TYPE_FCALL);

    // build aggregator
    auto func = static_cast<Function*>(expression->getData());
    TRI_ASSERT(func != nullptr);

    // function should have one argument (an array with the parameters)
    TRI_ASSERT(expression->numMembers() == 1);

    auto args = expression->getMember(0);
    // the number of arguments should also be one (note: this has been
    // validated before)
    TRI_ASSERT(args->type == NODE_TYPE_ARRAY);
    std::string_view functionName = Aggregator::translateAlias(func->name);
    Variable const* variable = nullptr;
    if (args->numMembers() == 1) {
      auto arg = args->getMember(0);
      if (arg->type == NODE_TYPE_REFERENCE) {
        // operand is a variable
        variable = static_cast<Variable*>(arg->getData());
      } else {
        auto calc = createTemporaryCalculation(arg, *previous);
        *previous = calc;
        variable = getOutVariable(calc);
      }
    } else {
      TRI_ASSERT(!Aggregator::requiresInput(func->name));
    }
    aggregateVariables.emplace_back(
        AggregateVarInfo{outVar, variable, std::string(functionName)});
  }

  return aggregateVariables;
}

AstNode const* ExecutionPlan::resolveVariableAlias(AstNode const* node) const {
  if (node->type == NODE_TYPE_REFERENCE) {
    auto setter =
        getVarSetBy(static_cast<Variable const*>(node->getData())->id);
    if (setter != nullptr && setter->getType() == ExecutionNode::CALCULATION) {
      auto cn = ExecutionNode::castTo<CalculationNode const*>(setter);
      node = cn->expression()->node();
    }
  }
  return node;
}

IndexHint ExecutionPlan::firstUnsatisfiedForcedIndexHint() const {
  if (hasForcedIndexHints()) {
    // plan contains forced index hints.
    // now check if they are all satisfied.
    containers::SmallVector<ExecutionNode*, 8> nodes;
    findNodesOfType(nodes, ::indexHintCheckTypes, true);
    for (auto n : nodes) {
      TRI_ASSERT(n);
      TRI_ASSERT(n->getType() == ExecutionNode::ENUMERATE_COLLECTION ||
                 n->getType() == ExecutionNode::TRAVERSAL ||
                 n->getType() == ExecutionNode::ENUMERATE_PATHS);

      auto const& hint = [](ExecutionNode const* n) {
        // extract indexHint from node
        if (n->getType() == ExecutionNode::ENUMERATE_COLLECTION) {
          return ExecutionNode::castTo<EnumerateCollectionNode const*>(n)
              ->hint();
        }
#ifdef EXTENDED_INDEX_HINTS_FOR_GRAPH_OPERATIONS
        if (n->getType() == ExecutionNode::TRAVERSAL) {
          return ExecutionNode::castTo<TraversalNode const*>(n)->hint();
        }
        if (n->getType() == ExecutionNode::ENUMERATE_PATHS) {
          return ExecutionNode::castTo<EnumeratePathsNode const*>(n)->hint();
        }
#endif
        TRI_ASSERT(false);
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_INTERNAL,
            absl::StrCat("invalid node type encountered with forcedIndexHint: ",
                         n->getTypeString()));
      }(n);

      if (hint.isSet() && hint.isForced()) {
        // unsatisfied index hint.
        return hint;
      }
    }
  }

  return IndexHint{};
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
/// @brief show an overview over the plan
struct Shower final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
  int indent;

  Shower() : indent(0) {}

  ~Shower() override = default;

  bool enterSubquery(ExecutionNode*, ExecutionNode*) final {
    indent++;
    return true;
  }

  void leaveSubquery(ExecutionNode*, ExecutionNode*) final { indent--; }

  bool before(ExecutionNode* en) final { return false; }

  void after(ExecutionNode* en) final {
    if (en->getType() == ExecutionNode::SUBQUERY_END) {
      --indent;
    }

    LoggerStream logLn;
    logLn << Logger::LOGID("24fa8") << LogLevel::INFO << Logger::AQL;

    for (int i = 0; i < 2 * indent; i++) {
      logLn << ' ';
    }
    logNode(logLn, *en);

    if (en->getType() == ExecutionNode::SUBQUERY_START) {
      ++indent;
    }
  }

  static LoggerStreamBase& logNode(LoggerStreamBase& log,
                                   ExecutionNode const& node) {
    return log << "[" << node.id() << "] " << node.getTypeString()
               << nodeDetails(node);
  }

  static std::string nodeDetails(ExecutionNode const& node) {
    std::string result;

    switch (node.getType()) {
      case ExecutionNode::CALCULATION: {
        auto const& calcNode =
            *ExecutionNode::castTo<CalculationNode const*>(&node);
        absl::StrAppend(&result, " $", calcNode.outVariable()->id, " = ");
        calcNode.expression()->stringify(result);
        break;
      }
      case ExecutionNode::FILTER: {
        auto const& filterNode =
            *ExecutionNode::castTo<FilterNode const*>(&node);
        absl::StrAppend(&result, " $", filterNode.inVariable()->id);
        break;
      }
      case ExecutionNode::SORT: {
        auto const& sortNode = *ExecutionNode::castTo<SortNode const*>(&node);
        for (auto const& it : sortNode.elements()) {
          absl::StrAppend(&result, " ", it.toString());
        }
        break;
      }
      case ExecutionNode::TRAVERSAL:
      case ExecutionNode::SHORTEST_PATH:
      case ExecutionNode::ENUMERATE_PATHS: {
        auto const& graphNode = *ExecutionNode::castTo<GraphNode const*>(&node);
        if (graphNode.isUsedAsSatellite()) {
          absl::StrAppend(&result, " used as satellite");
        }
        break;
      }
      case ExecutionNode::ENUMERATE_NEAR_VECTORS: {
        auto* enumNode =
            ExecutionNode::castTo<EnumerateNearVectorNode const*>(&node);
        absl::StrAppend(&result, " $", enumNode->documentOutVariable()->id,
                        " NEAR $", enumNode->inVariable()->id);
      } break;
      case ExecutionNode::INDEX: {
        auto* indexNode = ExecutionNode::castTo<IndexNode const*>(&node);
        absl::StrAppend(&result, " ", indexNode->collection()->name(), " -> ",
                        indexNode->outVariable()->name);
        if (indexNode->condition() != nullptr &&
            indexNode->condition()->root() != nullptr) {
          absl::StrAppend(&result, " ",
                          indexNode->condition()->root()->toString());
        }
        break;
      }
      case ExecutionNode::ENUMERATE_COLLECTION:
        absl::StrAppend(
            &result, " -> $",
            ExecutionNode::castTo<EnumerateCollectionNode const*>(&node)
                ->outVariable()
                ->id);
        [[fallthrough]];
      case ExecutionNode::UPDATE:
      case ExecutionNode::INSERT:
      case ExecutionNode::REMOVE:
      case ExecutionNode::REPLACE:
      case ExecutionNode::UPSERT: {
        auto const& colAccess =
            *ExecutionNode::castTo<CollectionAccessingNode const*>(&node);
        absl::StrAppend(&result, " (", colAccess.collection()->name(), ")");
        if (colAccess.isUsedAsSatellite()) {
          absl::StrAppend(&result, " used as satellite");
        }
        break;
      }
      default:
        break;
    }

    switch (node.getType()) {
      case ExecutionNode::ENUMERATE_COLLECTION:
      case ExecutionNode::INDEX: {
        auto* docNode = dynamic_cast<DocumentProducingNode const*>(&node);
        TRI_ASSERT(docNode != nullptr);
        Expression* filter = docNode->filter();
        if (filter != nullptr) {
          absl::StrAppend(&result, " filter: ", filter->node()->toString());
        }
        break;
      }
      default: {
      }
    }

    return result;
  }
};

/// @brief show an overview over the plan
void ExecutionPlan::show() const {
  Shower shower;
  _root->flatWalk(shower, false);
}

#endif
