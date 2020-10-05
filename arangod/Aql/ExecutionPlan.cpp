////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#include <Graph/TraverserOptions.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

#include "ExecutionPlan.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Aggregator.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/CollectNode.h"
#include "Aql/CollectOptions.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionNode.h"
#include "Aql/Expression.h"
#include "Aql/Function.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/IndexHint.h"
#include "Aql/KShortestPathsNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/NodeFinder.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/Query.h"
#include "Aql/ReadAllNode.h"
#include "Aql/RegisterPlan.h"
#include "Aql/ShortestPathNode.h"
#include "Aql/SortNode.h"
#include "Aql/TraversalNode.h"
#include "Aql/VarUsageFinder.h"
#include "Aql/Variable.h"
#include "Aql/WalkerWorker.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Containers/SmallVector.h"
#include "Graph/ShortestPathOptions.h"
#include "Graph/TraverserOptions.h"
#include "Logger/LoggerStream.h"
#include "Utils/OperationOptions.h"
#include "VocBase/AccessMode.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace {

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
/// @brief validate the counters of the plan
struct NodeCounter final : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
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
    if (en->getType() == ExecutionNode::MUTEX &&
        seen.find(en) != seen.end()) {
      return true;
    }
    if (!arangodb::ServerState::instance()->isDBServer() ||
        (en->getType() != ExecutionNode::REMOTE && en->getType() != ExecutionNode::SCATTER &&
         en->getType() != ExecutionNode::DISTRIBUTE)) {
      return WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique>::done(en);
    }
    return false;
  }
};
#endif

uint64_t checkTraversalDepthValue(AstNode const* node) {
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

void parseGraphCollectionRestriction(std::vector<std::string>& collections,
                                     AstNode const* src) {
  if (src->isStringValue()) {
    collections.emplace_back(src->getString());
  } else if (src->type == NODE_TYPE_ARRAY) {
    size_t const n = src->numMembers();
    collections.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      AstNode const* c = src->getMemberUnchecked(i);
      if (!c->isStringValue()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_BAD_PARAMETER,
            "collection restrictions option must be either a string or an "
            "array of collection names");
      }
      collections.emplace_back(c->getStringValue(), c->getStringLength());
    }
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "collection restrictions option must be either a string or an array of "
        "collection names");
  }
}

std::unique_ptr<graph::BaseOptions> createTraversalOptions(Ast* ast,
                                                           AstNode const* direction,
                                                           AstNode const* optionsNode) {
  aql::QueryContext& query = ast->query();
  auto options = std::make_unique<traverser::TraverserOptions>(query);

  TRI_ASSERT(direction != nullptr);
  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);
  auto steps = direction->getMember(1);

  bool invalidDepth = false;

  if (steps->isNumericValue()) {
    // Check if a double value is integer
    options->minDepth = checkTraversalDepthValue(steps);
    options->maxDepth = options->minDepth;
  } else if (steps->type == NODE_TYPE_RANGE) {
    // Range depth
    options->minDepth = checkTraversalDepthValue(steps->getMember(0));
    options->maxDepth = checkTraversalDepthValue(steps->getMember(1));

    if (options->maxDepth < options->minDepth) {
      invalidDepth = true;
    }
  } else {
    invalidDepth = true;
  }

  if (invalidDepth) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }

  // WTF this is the 10th place the deals with parsing of options
  // how do you even maintain that?
  if (optionsNode != nullptr && optionsNode->type == NODE_TYPE_OBJECT) {
    size_t n = optionsNode->numMembers();
    bool hasBFS = false;

    for (size_t i = 0; i < n; ++i) {
      auto member = optionsNode->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringRef();
        auto value = member->getMember(0);
        TRI_ASSERT(value->isConstant());

        if (name == "bfs") {
          options->mode = value->isTrue()
                              ? arangodb::traverser::TraverserOptions::Order::BFS
                              : arangodb::traverser::TraverserOptions::Order::DFS;
          hasBFS = true;
        } else if (name == "uniqueVertices" && value->isStringValue()) {
          if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryPath)) {
            options->uniqueVertices =
                arangodb::traverser::TraverserOptions::UniquenessLevel::PATH;
          } else if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryGlobal)) {
            options->uniqueVertices =
                arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL;
          }
        } else if (name == "uniqueEdges" && value->isStringValue()) {
          // path is the default
          if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryNone)) {
            options->uniqueEdges =
                arangodb::traverser::TraverserOptions::UniquenessLevel::NONE;
          } else if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryGlobal)) {
            THROW_ARANGO_EXCEPTION_MESSAGE(
                TRI_ERROR_BAD_PARAMETER,
                "uniqueEdges: 'global' is not supported, "
                "due to otherwise unpredictable results. Use 'path' "
                "or 'none' instead");
          }
        } else if (name == "edgeCollections") {
          parseGraphCollectionRestriction(options->edgeCollections, value);
        } else if (name == "vertexCollections") {
          parseGraphCollectionRestriction(options->vertexCollections, value);
        } else if (name == StaticStrings::GraphQueryOrder && !hasBFS) {
          // dfs is the default
          if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryOrderBFS)) {
            options->mode = traverser::TraverserOptions::Order::BFS;
          } else if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryOrderWeighted)) {
            options->mode = traverser::TraverserOptions::Order::WEIGHTED;
          } else if (value->stringEqualsCaseInsensitive(StaticStrings::GraphQueryOrderDFS)) {
            options->mode = traverser::TraverserOptions::Order::DFS;
          } else {
            THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                           "order: unknown order");
          }
        } else if (name == "defaultWeight" && value->isNumericValue()) {
          options->defaultWeight = value->getDoubleValue();
        } else if (name == "weightAttribute" && value->isStringValue()) {
          options->weightAttribute = value->getString();
        } else if (name == "parallelism") {
          if (ast->canApplyParallelism()) {
            // parallelism is only used when there is no usage of V8 in the
            // query and if the query is not a modification query.
            options->setParallelism(Ast::validatedParallelism(value));
          }
        }
      }
    }
  }

  if (options->uniqueVertices == arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL &&
      !(options->isUniqueGlobalVerticesAllowed())) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "uniqueVertices: 'global' is only "
                                   "supported, with mode: bfs|weighted due to "
                                   "otherwise unpredictable results.");
  }

  return options;
}

std::unique_ptr<graph::BaseOptions> createShortestPathOptions(arangodb::aql::QueryContext& query,
                                                              AstNode const* node) {
  auto options = std::make_unique<graph::ShortestPathOptions>(query);

  if (node != nullptr && node->type == NODE_TYPE_OBJECT) {
    size_t n = node->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = node->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringRef();
        auto value = member->getMember(0);

        TRI_ASSERT(value->isConstant());

        if (name == "weightAttribute" && value->isStringValue()) {
          options->weightAttribute =
              std::string(value->getStringValue(), value->getStringLength());
        } else if (name == "defaultWeight" && value->isNumericValue()) {
          options->defaultWeight = value->getDoubleValue();
        }
      }
    }
  }

  return options;
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
ExecutionPlan::ExecutionPlan(Ast* ast)
    : _ids(),
      _root(nullptr),
      _planValid(true),
      _varUsageComputed(false),
      _nestingLevel(0),
      _nextId(0),
      _ast(ast),
      _lastLimitNode(nullptr),
      _subqueries(),
      _typeCounts{} {}

/// @brief destroy the plan, frees all assigned nodes
ExecutionPlan::~ExecutionPlan() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // On coordinator there are temporary nodes injected into the Plan, that are NOT part
  // of the full execution tree. This is in order to reuse a lot of the nodes
  // for each DBServer instead of having all nodes copy everything once again.
  if (_root != nullptr && _planValid && !arangodb::ServerState::instance()->isCoordinator()) {
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

#ifndef ARANGODB_USE_GOOGLE_TESTS
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // we are in the destructor here already. decreasing the memory usage counters
  // will only provide a benefit (in terms of assertions) if we are in
  // maintainer mode, so we can save all these operations in non-maintainer mode
  _ast->query().resourceMonitor().decreaseMemoryUsage(_ids.size() * sizeof(ExecutionNode));
#endif
#endif

  for (auto& x : _ids) {
    delete x.second;
  }
}

/// @brief create an execution plan from an AST
std::unique_ptr<ExecutionPlan> ExecutionPlan::instantiateFromAst(Ast* ast) {
  TRI_ASSERT(ast != nullptr);

  auto root = ast->root();
  TRI_ASSERT(root != nullptr);
  TRI_ASSERT(root->type == NODE_TYPE_ROOT);

  auto plan = std::make_unique<ExecutionPlan>(ast);

  plan->_root = plan->fromNode(root);

  // set fullCount flag for last LIMIT node on main level
  if (plan->_lastLimitNode != nullptr && ast->query().queryOptions().fullCount) {
    ExecutionNode::castTo<LimitNode*>(plan->_lastLimitNode)->setFullCount();
  }

  // set count flag for final RETURN node
  if (plan->_root->getType() == ExecutionNode::RETURN) {
    ExecutionNode::castTo<ReturnNode*>(plan->_root)->setCount();
  }

  plan->findVarUsage();

  return plan;
}

bool ExecutionPlan::contains(ExecutionNode::NodeType type) const {
  TRI_ASSERT(_varUsageComputed);
  return _typeCounts[type] > 0;
}

/// @brief increase the node counter for the type
void ExecutionPlan::increaseCounter(ExecutionNode::NodeType type) noexcept {
  ++_typeCounts[type];
}

/// @brief process the list of collections in a VelocyPack
void ExecutionPlan::getCollectionsFromVelocyPack(aql::Collections& colls, VPackSlice const slice) {
  VPackSlice collectionsSlice = slice;
  if (slice.isObject()) {
    collectionsSlice = slice.get("collections");
  }

  if (!collectionsSlice.isArray()) {
   THROW_ARANGO_EXCEPTION_MESSAGE(
       TRI_ERROR_INTERNAL,
       "json node \"collections\" not found or not an array");
  }

  for (auto const& collection : VPackArrayIterator(collectionsSlice)) {
    colls.add(
        basics::VelocyPackHelper::checkAndGetStringValue(collection, "name"),
        AccessMode::fromString(arangodb::basics::VelocyPackHelper::checkAndGetStringValue(collection,
                                                                                          "type")
                                   .c_str()),
        aql::Collection::Hint::Shard
    );
  }
}

/// @brief create an execution plan from VelocyPack
std::unique_ptr<ExecutionPlan> ExecutionPlan::instantiateFromVelocyPack(Ast* ast, VPackSlice const slice) {
  TRI_ASSERT(ast != nullptr);

  auto plan = std::make_unique<ExecutionPlan>(ast);
  plan->_root = plan->fromSlice(slice);
  plan->setVarUsageComputed();

  return plan;
}

/// @brief clone an existing execution plan
ExecutionPlan* ExecutionPlan::clone(Ast* ast) {
  auto plan = std::make_unique<ExecutionPlan>(ast);
  plan->_nextId = _nextId;
  plan->_root = _root->clone(plan.get(), true, false);
  plan->_appliedRules = _appliedRules;
  plan->_disabledRules = _disabledRules;
  plan->_nestingLevel = _nestingLevel;

  return plan.release();
}

/// @brief clone an existing execution plan
ExecutionPlan* ExecutionPlan::clone() { return clone(_ast); }

/// @brief export to VelocyPack
std::shared_ptr<VPackBuilder> ExecutionPlan::toVelocyPack(Ast* ast, bool verbose,
                                                          ExplainRegisterPlan explainRegisterPlan) const {
  VPackOptions options;
  options.checkAttributeUniqueness = false;
  options.buildUnindexedArrays = true;
  auto builder = std::make_shared<VPackBuilder>(&options);

  toVelocyPack(*builder, ast, verbose, explainRegisterPlan);
  return builder;
}

/// @brief export to VelocyPack
void ExecutionPlan::toVelocyPack(VPackBuilder& builder, Ast* ast, bool verbose,
                                 ExplainRegisterPlan explainRegisterPlan) const {
  unsigned flags = ExecutionNode::SERIALIZE_ESTIMATES;
  if (verbose) {
    flags |= ExecutionNode::SERIALIZE_PARENTS | ExecutionNode::SERIALIZE_DETAILS |
             ExecutionNode::SERIALIZE_FUNCTIONS;
  }
  if (explainRegisterPlan == ExplainRegisterPlan::Yes) {
    flags |= ExecutionNode::SERIALIZE_REGISTER_INFORMATION;
  }
  // keeps top level of built object open
  _root->toVelocyPack(builder, flags, true);

  TRI_ASSERT(!builder.isClosed());

  // set up rules
  builder.add(VPackValue("rules"));
  builder.openArray();
  for (auto const& ruleName : OptimizerRulesFeature::translateRules(_appliedRules)) {
    builder.add(VPackValuePair(ruleName.data(), ruleName.size(), VPackValueType::String));
  }
  builder.close();

  // set up collections
  builder.add(VPackValue("collections"));
  ast->query().collections().toVelocyPack(builder);

  // set up variables
  builder.add(VPackValue("variables"));
  ast->variables()->toVelocyPack(builder);

  CostEstimate estimate = _root->getCost();
  // simon: who is reading this ?
  builder.add("estimatedCost", VPackValue(estimate.estimatedCost));
  builder.add("estimatedNrItems", VPackValue(estimate.estimatedNrItems));
  builder.add("isModificationQuery", VPackValue(ast->containsModificationNode()));

  builder.close();
}

void ExecutionPlan::addAppliedRule(int level) {
  if (_appliedRules.empty() || _appliedRules.back() != level) {
    _appliedRules.emplace_back(level);
  }
}

bool ExecutionPlan::hasAppliedRule(int level) const {
  return std::any_of(_appliedRules.begin(), _appliedRules.end(),
                     [level](int l) { return l == level; });
}

void ExecutionPlan::enableRule(int rule) { _disabledRules.erase(rule); }

void ExecutionPlan::disableRule(int rule) { _disabledRules.emplace(rule); }

bool ExecutionPlan::isDisabledRule(int rule) const {
  return (_disabledRules.find(rule) != _disabledRules.end());
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

  std::string msg = std::string("node [") + std::to_string(id.id()) +
                    std::string("] wasn't found");
  // node unknown
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
}

std::unordered_map<ExecutionNodeId, ExecutionNode*> const& ExecutionPlan::getNodesById() const {
  return _ids;
}

/// @brief creates a calculation node for an arbitrary expression
ExecutionNode* ExecutionPlan::createCalculation(Variable* out, AstNode const* expression,
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
          args->changeMember(i, _ast->createNodeValueString(member->getStringValue(),
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
        // create an on-the-fly subquery for a full collection access
        AstNode* rootNode = _ast->createNodeSubquery();

        // FOR part
        Variable* v = _ast->variables()->createTemporaryVariable();
        AstNode* forNode = _ast->createNodeFor(v, node, nullptr);
        // RETURN part
        AstNode* returnNode = _ast->createNodeReturn(_ast->createNodeReference(v));

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
        auto en = registerNode(new SubqueryNode(this, nextId(), subquery, v));
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
  CalculationNode* en = new CalculationNode(this, nextId(), std::move(expr), out);

  registerNode(reinterpret_cast<ExecutionNode*>(en));

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
SubqueryNode* ExecutionPlan::getSubqueryFromExpression(AstNode const* expression) const {
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
    // CollectNode has an outVariale() method, but we cannot use it.
    // for CollectNode, outVariable() will return the variable filled by INTO,
    // but INTO is an optional feature
    // so this will return the first result variable of the COLLECT
    // this part of the code should only be called for anonymous COLLECT nodes,
    // which only have one result variable
    auto en = ExecutionNode::castTo<CollectNode const*>(node);
    auto const& vars = en->groupVariables();

    TRI_ASSERT(vars.size() == 1);
    auto v = vars[0].first;
    TRI_ASSERT(v != nullptr);
    return v;
  }

  if (node->getType() == ExecutionNode::SUBQUERY) {
    return ExecutionNode::castTo<SubqueryNode const*>(node)->outVariable();
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 std::string("invalid node type '") + node->getTypeString() +
                                     "' in getOutVariable");
}

/// @brief creates an anonymous COLLECT node (for a DISTINCT)
CollectNode* ExecutionPlan::createAnonymousCollect(CalculationNode const* previous) {
  // generate an out variable for the COLLECT
  auto out = _ast->variables()->createTemporaryVariable();
  TRI_ASSERT(out != nullptr);

  std::vector<std::pair<Variable const*, Variable const*>> const groupVariables{
      std::make_pair(out, previous->outVariable())};
  std::vector<std::pair<Variable const*, std::pair<Variable const*, std::string>>> const aggregateVariables{};

  auto en = new CollectNode(this, nextId(), CollectOptions(), groupVariables, aggregateVariables,
                            nullptr, nullptr, std::vector<Variable const*>(),
                            _ast->variables()->variables(false), false, true);

  registerNode(en);
  en->aggregationMethod(CollectOptions::CollectMethod::DISTINCT);
  en->specialized();

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
      auto const name = member->getStringRef();

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
ModificationOptions ExecutionPlan::parseModificationOptions(AstNode const* node) {
  ModificationOptions options;

  // parse the modification options we got
  if (node != nullptr && node->type == NODE_TYPE_OBJECT) {
    size_t n = node->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = node->getMemberUnchecked(i);

      if (member != nullptr && member->type == NODE_TYPE_OBJECT_ELEMENT) {
        auto const name = member->getStringRef();
        auto value = member->getMember(0);

        TRI_ASSERT(value->isConstant());

        if (name == StaticStrings::WaitForSyncString) {
          options.waitForSync = value->isTrue();
        } else if (name == StaticStrings::SkipDocumentValidation) {
          options.validate = !value->isTrue();
        } else if (name == StaticStrings::KeepNullString) {
          options.keepNull = value->isTrue();
        } else if (name == StaticStrings::MergeObjectsString) {
          options.mergeObjects = value->isTrue();
        } else if (name == StaticStrings::Overwrite && value->isTrue()) {
          // legacy: overwrite is set, superseded by overwriteMode
          // default behavior if only "overwrite" is specified
          if (!options.isOverwriteModeSet()) {
            options.overwriteMode = OperationOptions::OverwriteMode::Replace;
          }
        } else if (name == StaticStrings::OverwriteMode && value->isStringValue()) {
          auto overwriteMode =
              OperationOptions::determineOverwriteMode(value->getStringRef());

          if (overwriteMode != OperationOptions::OverwriteMode::Unknown) {
            options.overwriteMode = overwriteMode;
          }
        } else if (name == StaticStrings::IgnoreRevsString) {
          options.ignoreRevs = value->isTrue();
        } else if (name == "exclusive") {
          options.exclusive = value->isTrue();
        } else if (name == "ignoreErrors") {
          options.ignoreErrors = value->isTrue();
        }
      }
    }
  }

  return options;
}

/// @brief create modification options from an AST node
ModificationOptions ExecutionPlan::createModificationOptions(AstNode const* node) {
  ModificationOptions options = parseModificationOptions(node);

  // this means a data-modification query must first read the entire input data
  // before starting with the modifications
  // this is safe in all cases
  // the flag can be set to false later to execute read and write operations in
  // lockstep
  options.readCompleteInput = true;

  if (!_ast->functionsMayAccessDocuments()) {
    // no functions in the query can access document data...
    bool isReadWrite = false;

    if (_ast->containsTraversal()) {
      // its unclear which collections the traversal will access
      isReadWrite = true;
    } else {
      _ast->query().collections().visit([&isReadWrite](std::string const&, aql::Collection& collection) {
        if (collection.isReadWrite()) {
          // stop iterating
          isReadWrite = true;
          return false;
        }
        return true;
      });
    }

    if (!isReadWrite) {
      // no collection is used in both read and write mode
      // this means the query's write operation can use read & write in lockstep
      options.readCompleteInput = false;
    }
  }

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
        auto const name = member->getStringRef();
        if (name == "method") {
          auto value = member->getMember(0);
          if (value->isStringValue()) {
            options.method = CollectOptions::methodFromString(value->getString());
          }
        }
      }
    }
  }

  return options;
}

/// @brief register a node with the plan
ExecutionNode* ExecutionPlan::registerNode(std::unique_ptr<ExecutionNode> node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->plan() == this);
  TRI_ASSERT(node->id() > ExecutionNodeId{0});
  TRI_ASSERT(_ids.find(node->id()) == _ids.end());

  // may throw
  _ast->query().resourceMonitor().increaseMemoryUsage(sizeof(ExecutionNode));

  try {
    auto emplaced = _ids.try_emplace(node->id(), node.get()).second;  // take ownership
    TRI_ASSERT(emplaced);
    return node.release();
  } catch (...) {
    // clean up
    _ast->query().resourceMonitor().decreaseMemoryUsage(sizeof(ExecutionNode));
    throw;
  }
}

/// @brief register a node with the plan, will delete node if addition fails
ExecutionNode* ExecutionPlan::registerNode(ExecutionNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->plan() == this);
  TRI_ASSERT(node->id() > ExecutionNodeId{0});
  TRI_ASSERT(_ids.find(node->id()) == _ids.end());

  try {
    // may throw
    _ast->query().resourceMonitor().increaseMemoryUsage(sizeof(ExecutionNode));

    auto [it, emplaced] = _ids.try_emplace(node->id(), node);
    if (!emplaced) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unable to register node in plan");
    }
    TRI_ASSERT(it != _ids.end());
  } catch (...) {
    delete node;
    throw;
  }

  return node;
}

SubqueryNode* ExecutionPlan::registerSubquery(SubqueryNode* node) {
  node = ExecutionNode::castTo<SubqueryNode*>(registerNode(node));
  _subqueries[node->outVariable()->id] = node;
  return node;
}

/// @brief creates a anonymous calculation node for an arbitrary expression
ExecutionNode* ExecutionPlan::createTemporaryCalculation(AstNode const* expression,
                                                         ExecutionNode* previous) {
  // generate a temporary variable
  auto out = _ast->variables()->createTemporaryVariable();
  TRI_ASSERT(out != nullptr);

  return createCalculation(out, expression, previous);
}

/// @brief adds "previous" as dependency to "plan", returns "plan"
ExecutionNode* ExecutionPlan::addDependency(ExecutionNode* previous, ExecutionNode* plan) {
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

/// @brief create an execution plan element from an AST FOR (non-view) node
ExecutionNode* ExecutionPlan::fromNodeFor(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_FOR);
  TRI_ASSERT(node->numMembers() == 3);

  auto variable = node->getMember(0);
  auto expression = node->getMember(1);
  auto options = node->getMember(2);
  IndexHint hint(options);

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
    en = registerNode(new EnumerateCollectionNode(this, nextId(), collection, v, false, hint));
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
      view = vocbase.server().getFeature<ClusterFeature>().clusterInfo().getView(
          vocbase.name(), viewName);
    }

    if (!view) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "no view for EnumerateView");
    }

    auto* options = node->getMemberUnchecked(2);
    if (options->type == NODE_TYPE_NOP) {
      options = nullptr;
    }

    en = registerNode(new iresearch::IResearchViewNode(*this, nextId(), vocbase, view,
                                                       *v, nullptr, options, {}));
  } else if (expression->type == NODE_TYPE_REFERENCE) {
    // second operand is already a variable
    auto inVariable = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(inVariable != nullptr);
    en = registerNode(new EnumerateListNode(this, nextId(), inVariable, v));
  } else {
    // second operand is some misc. expression
    auto calc = createTemporaryCalculation(expression, previous);
    en = registerNode(new EnumerateListNode(this, nextId(), getOutVariable(calc), v));
    previous = calc;
  }

  TRI_ASSERT(en != nullptr);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST FOR (view) node
ExecutionNode* ExecutionPlan::fromNodeForView(ExecutionNode* previous, AstNode const* node) {
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

      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                     std::string(TRI_errno_string(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) +
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

  en = registerNode(new iresearch::IResearchViewNode(*this, nextId(), vocbase, view, *v,
                                                     search->getMember(0), options, {}));

  TRI_ASSERT(en != nullptr);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST FOR TRAVERSAL node
ExecutionNode* ExecutionPlan::fromNodeTraversal(ExecutionNode* previous, AstNode const* node) {
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
          member->getString() == StaticStrings::IdString) {
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

  auto options =
      createTraversalOptions(getAst(), direction, node->getMember(4));

  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);
  direction = direction->getMember(0);
  TRI_ASSERT(direction->isIntValue());

  // First create the node
  auto travNode =
      new TraversalNode(this, nextId(), &(_ast->query().vocbase()), direction, start,
                        graph, std::move(pruneExpression), std::move(options));

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

  ExecutionNode* en = registerNode(travNode);
  TRI_ASSERT(en != nullptr);
  return addDependency(previous, en);
}

AstNode const* ExecutionPlan::parseTraversalVertexNode(ExecutionNode*& previous,
                                                       AstNode const* vertex) {
  if (vertex->type == NODE_TYPE_OBJECT && vertex->isConstant()) {
    size_t n = vertex->numMembers();
    for (size_t i = 0; i < n; ++i) {
      auto member = vertex->getMember(i);
      if (member->type == NODE_TYPE_OBJECT_ELEMENT &&
          member->getString() == StaticStrings::IdString) {
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
  TRI_ASSERT(direction->isIntValue());
  AstNode const* start = parseTraversalVertexNode(previous, node->getMember(1));
  AstNode const* target = parseTraversalVertexNode(previous, node->getMember(2));
  AstNode const* graph = node->getMember(3);

  auto options = createShortestPathOptions(_ast->query(), node->getMember(4));

  // First create the node
  auto spNode = new ShortestPathNode(this, nextId(), &(_ast->query().vocbase()), direction,
                                     start, target, graph, std::move(options));

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

  ExecutionNode* en = registerNode(spNode);
  TRI_ASSERT(en != nullptr);
  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST for SHORTEST_PATH node
ExecutionNode* ExecutionPlan::fromNodeKShortestPaths(ExecutionNode* previous,
                                                     AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_K_SHORTEST_PATHS);
  TRI_ASSERT(node->numMembers() >= 6);
  TRI_ASSERT(node->numMembers() <= 7);

  // the first 4 members are used by shortest_path internally.
  // The members 5-6, where 6 is optional, are used
  // as out variables.
  AstNode const* direction = node->getMember(0);
  TRI_ASSERT(direction->isIntValue());
  AstNode const* start = parseTraversalVertexNode(previous, node->getMember(1));
  AstNode const* target = parseTraversalVertexNode(previous, node->getMember(2));
  AstNode const* graph = node->getMember(3);

  // FIXME: here goes the parameters with k etc
  auto options = createShortestPathOptions(getAst()->query(), node->getMember(4));

  // First create the node
  auto spNode = new KShortestPathsNode(this, nextId(), &(_ast->query().vocbase()), direction,
                                       start, target, graph, std::move(options));

  auto variable = node->getMember(5);
  TRI_ASSERT(variable->type == NODE_TYPE_VARIABLE);
  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);
  spNode->setPathOutput(v);

  ExecutionNode* en = registerNode(spNode);
  TRI_ASSERT(en != nullptr);
  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST FILTER node
ExecutionNode* ExecutionPlan::fromNodeFilter(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_FILTER);
  TRI_ASSERT(node->numMembers() == 1);

  auto expression = node->getMember(0);

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(v != nullptr);
    en = registerNode(new FilterNode(this, nextId(), v));
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
      en = registerNode(new NoResultsNode(this, nextId()));
    } else {
      auto calc = createTemporaryCalculation(expression, previous);
      en = registerNode(new FilterNode(this, nextId(), getOutVariable(calc)));
      previous = calc;
    }
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST LET node
/// this also includes handling of subqueries (as subqueries can only occur
/// inside LET nodes)
ExecutionNode* ExecutionPlan::fromNodeLet(ExecutionNode* previous, AstNode const* node) {
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

    en = registerNode(new SubqueryNode(this, nextId(), subquery, v));
    _subqueries[ExecutionNode::castTo<SubqueryNode*>(en)->outVariable()->id] = en;
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
ExecutionNode* ExecutionPlan::fromNodeSort(ExecutionNode* previous, AstNode const* node) {
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
        if (ascending->stringEqualsCaseInsensitive(StaticStrings::QuerySortASC)) {
          isAscending = true;
          handled = true;
        } else if (ascending->stringEqualsCaseInsensitive(StaticStrings::QuerySortDESC)) {
          isAscending = false;
          handled = true;
        }
      }
    }

    if (!handled) {
      // if no sort order is set, ensure we have one
      AstNode const* ascendingNode = ascending->castToBool(_ast);
      if (ascendingNode->type == NODE_TYPE_VALUE && ascendingNode->value.type == VALUE_TYPE_BOOL) {
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
      elements.emplace_back(v, isAscending);
    } else {
      // sort operand is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      elements.emplace_back(getOutVariable(calc), isAscending);
      previous = calc;
    }
  }

  if (elements.empty()) {
    // no sort criterion remained - this can only happen if all sort
    // criteria were constants
    return previous;
  }

  // at least one sort criterion remained
  auto en = registerNode(new SortNode(this, nextId(), elements, false));

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST COLLECT node
ExecutionNode* ExecutionPlan::fromNodeCollect(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_COLLECT);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createCollectOptions(node->getMember(0));

  auto groups = node->getMember(1);
  size_t const numVars = groups->numMembers();

  std::vector<std::pair<Variable const*, Variable const*>> groupVariables;
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
      groupVariables.emplace_back(std::make_pair(v, e));
    } else {
      // operand is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      previous = calc;
      groupVariables.emplace_back(std::make_pair(v, getOutVariable(calc)));
    }
  }

  // aggregate variables
  std::vector<std::pair<Variable const*, std::pair<Variable const*, std::string>>> aggregateVariables;
  {
    auto list = node->getMember(2);
    TRI_ASSERT(list->type == NODE_TYPE_AGGREGATIONS);
    list = list->getMember(0);
    TRI_ASSERT(list->type == NODE_TYPE_ARRAY);
    size_t const numVars = list->numMembers();

    aggregateVariables.reserve(numVars);
    for (size_t i = 0; i < numVars; ++i) {
      auto assigner = list->getMember(i);

      if (assigner == nullptr) {
        continue;
      }

      TRI_ASSERT(assigner->type == NODE_TYPE_ASSIGN);
      auto out = assigner->getMember(0);
      TRI_ASSERT(out != nullptr);
      auto v = static_cast<Variable*>(out->getData());
      TRI_ASSERT(v != nullptr);

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
      TRI_ASSERT(args->numMembers() == 1);

      auto arg = args->getMember(0);

      if (arg->type == NODE_TYPE_REFERENCE) {
        // operand is a variable
        auto e = static_cast<Variable*>(arg->getData());
        aggregateVariables.emplace_back(
            std::make_pair(v, std::make_pair(e, Aggregator::translateAlias(func->name))));
      } else {
        auto calc = createTemporaryCalculation(arg, previous);
        previous = calc;

        aggregateVariables.emplace_back(
            std::make_pair(v, std::make_pair(getOutVariable(calc),
                                             Aggregator::translateAlias(func->name))));
      }
    }
  }

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

  std::vector<Variable const*> keepVariables;
  auto const keep = node->getMember(5);

  if (keep->type != NODE_TYPE_NOP) {
    // variables to keep
    TRI_ASSERT(keep->type == NODE_TYPE_ARRAY);
    size_t const keepVarsSize = keep->numMembers();
    keepVariables.reserve(keepVarsSize);

    for (size_t i = 0; i < keepVarsSize; ++i) {
      auto ref = keep->getMember(i);
      TRI_ASSERT(ref->type == NODE_TYPE_REFERENCE);
      keepVariables.emplace_back(static_cast<Variable const*>(ref->getData()));
    }
  } else if (into->type != NODE_TYPE_NOP && expression->type == NODE_TYPE_NOP) {
    // `COLLECT ... INTO var ...` with neither an explicit expression
    // (INTO var = <expr>), nor an explicit `KEEP`.
    // In this case, we look for all previously defined user-defined variables
    // , which are in our scope, and which aren't top-level variables in the
    // (sub)query the COLLECT resides in.
    CollectNode::calculateAccessibleUserVariables(*previous, keepVariables);
  }

  auto collectNode =
      new CollectNode(this, nextId(), options, groupVariables, aggregateVariables,
                      expressionVariable, outVariable, keepVariables,
                      _ast->variables()->variables(false), false, false);

  auto en = registerNode(collectNode);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST COLLECT node, COUNT
/// note that also a sort plan node will be added in front of the collect plan
/// node
ExecutionNode* ExecutionPlan::fromNodeCollectCount(ExecutionNode* previous,
                                                   AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_COLLECT_COUNT);
  TRI_ASSERT(node->numMembers() == 3);

  auto options = createCollectOptions(node->getMember(0));

  auto list = node->getMember(1);
  size_t const numVars = list->numMembers();

  std::vector<std::pair<Variable const*, Variable const*>> groupVariables;
  groupVariables.reserve(numVars);
  for (size_t i = 0; i < numVars; ++i) {
    auto assigner = list->getMember(i);

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
      groupVariables.emplace_back(std::make_pair(v, e));
    } else {
      // operand is some misc expression
      auto calc = createTemporaryCalculation(expression, previous);
      previous = calc;
      groupVariables.emplace_back(std::make_pair(v, getOutVariable(calc)));
    }
  }

  // output variable
  auto v = node->getMember(2);
  // handle out variable
  Variable* outVariable = static_cast<Variable*>(v->getData());

  TRI_ASSERT(outVariable != nullptr);

  std::vector<std::pair<Variable const*, std::pair<Variable const*, std::string>>> const aggregateVariables{};

  auto collectNode =
      new CollectNode(this, nextId(), options, groupVariables, aggregateVariables,
                      nullptr, outVariable, std::vector<Variable const*>(),
                      _ast->variables()->variables(false), true, false);
  auto en = registerNode(collectNode);

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST LIMIT node
ExecutionNode* ExecutionPlan::fromNodeLimit(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_LIMIT);
  TRI_ASSERT(node->numMembers() == 2);

  auto offset = node->getMember(0);
  auto count = node->getMember(1);

  if (offset->type != NODE_TYPE_VALUE || count->type != NODE_TYPE_VALUE) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
        "LIMIT offset/count values must be constant numeric values");
  }

  TRI_ASSERT(offset->type == NODE_TYPE_VALUE);
  TRI_ASSERT(count->type == NODE_TYPE_VALUE);

  int64_t offsetValue = 0;
  if (offset->value.type != VALUE_TYPE_NULL) {
    if ((offset->value.type != VALUE_TYPE_INT && offset->value.type != VALUE_TYPE_DOUBLE) ||
        offset->getIntValue() < 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
          "LIMIT offset value is not a number or out of range");
    }
    offsetValue = offset->getIntValue();
  }

  int64_t countValue =
      128 * 1024 * 1024;  // arbitrary default value for an "unbounded" limit value
  if (count->value.type != VALUE_TYPE_NULL) {
    if ((count->value.type != VALUE_TYPE_INT && count->value.type != VALUE_TYPE_DOUBLE) ||
        count->getIntValue() < 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE,
          "LIMIT count value is not a number or out of range");
    }
    countValue = count->getIntValue();
  }

  auto en = registerNode(new LimitNode(this, nextId(), static_cast<size_t>(offsetValue),
                                       static_cast<size_t>(countValue)));

  if (_nestingLevel == 0) {
    _lastLimitNode = en;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST RETURN node
ExecutionNode* ExecutionPlan::fromNodeReturn(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_RETURN);
  TRI_ASSERT(node->numMembers() == 1);

  auto expression = node->getMember(0);

  ExecutionNode* en = nullptr;

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());
    TRI_ASSERT(v != nullptr);
    en = registerNode(new ReturnNode(this, nextId(), v));
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);
    en = registerNode(new ReturnNode(this, nextId(), getOutVariable(calc)));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST REMOVE node
ExecutionNode* ExecutionPlan::fromNodeRemove(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_REMOVE);
  TRI_ASSERT(node->numMembers() == 4);

  auto options = createModificationOptions(node->getMember(0));
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
  Variable const* outVariableOld = static_cast<Variable*>(returnVarNode->getData());

  if (expression->type == NODE_TYPE_REFERENCE) {
    // operand is already a variable
    auto v = static_cast<Variable*>(expression->getData());

    TRI_ASSERT(v != nullptr);
    en = registerNode(new RemoveNode(this, nextId(), collection, options, v, outVariableOld));
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);

    en = registerNode(new RemoveNode(this, nextId(), collection, options,
                                     getOutVariable(calc), outVariableOld));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST INSERT node
ExecutionNode* ExecutionPlan::fromNodeInsert(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_INSERT);
  TRI_ASSERT(node->numMembers() > 3);
  TRI_ASSERT(node->numMembers() < 6);

  auto options = createModificationOptions(node->getMember(0));
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
  Variable const* outVariableNew = static_cast<Variable*>(returnVarNode->getData());

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
    en = registerNode(new InsertNode(this, nextId(), collection, options, v,
                                     outVariableOld, outVariableNew));
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(expression, previous);

    en = registerNode(new InsertNode(this, nextId(), collection, options,
                                     getOutVariable(calc), outVariableOld, outVariableNew));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST UPDATE node
ExecutionNode* ExecutionPlan::fromNodeUpdate(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_UPDATE);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createModificationOptions(node->getMember(0));
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

  Variable const* outVariableOld = static_cast<Variable*>(node->getMember(4)->getData());
  Variable const* outVariableNew = static_cast<Variable*>(node->getMember(5)->getData());

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
    en = registerNode(new UpdateNode(this, nextId(), collection, options, v,
                                     keyVariable, outVariableOld, outVariableNew));
  } else {
    // document operand is some misc expression
    auto calc = createTemporaryCalculation(docExpression, previous);

    en = registerNode(new UpdateNode(this, nextId(), collection, options,
                                     getOutVariable(calc), keyVariable,
                                     outVariableOld, outVariableNew));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST REPLACE node
ExecutionNode* ExecutionPlan::fromNodeReplace(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_REPLACE);
  TRI_ASSERT(node->numMembers() == 6);

  auto options = createModificationOptions(node->getMember(0));
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

  Variable const* outVariableOld = static_cast<Variable*>(node->getMember(4)->getData());
  Variable const* outVariableNew = static_cast<Variable*>(node->getMember(5)->getData());

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
    en = registerNode(new ReplaceNode(this, nextId(), collection, options, v,
                                      keyVariable, outVariableOld, outVariableNew));
  } else {
    // operand is some misc expression
    auto calc = createTemporaryCalculation(docExpression, previous);

    en = registerNode(new ReplaceNode(this, nextId(), collection, options,
                                      getOutVariable(calc), keyVariable,
                                      outVariableOld, outVariableNew));
    previous = calc;
  }

  return addDependency(previous, en);
}

/// @brief create an execution plan element from an AST UPSERT node
ExecutionNode* ExecutionPlan::fromNodeUpsert(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_UPSERT);
  TRI_ASSERT(node->numMembers() == 7);

  auto options = createModificationOptions(node->getMember(0));
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

  auto docExpression = node->getMember(2);
  auto insertExpression = node->getMember(3);
  auto updateExpression = node->getMember(4);
  Variable const* outVariableNew = static_cast<Variable*>(node->getMember(6)->getData());

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

  bool isReplace = (node->getIntValue(true) == static_cast<int64_t>(NODE_TYPE_REPLACE));
  ExecutionNode* en =
      registerNode(new UpsertNode(this, nextId(), collection, options, docVariable,
                                  insertVar, updateVar, outVariableNew, isReplace));

  return addDependency(previous, en);
}


ExecutionNode* ExecutionPlan::fromNodeReadAll(ExecutionNode* previous, AstNode const* node) {
  TRI_ASSERT(node != nullptr && node->type == NODE_TYPE_READ_ALL);
  TRI_ASSERT(node->numMembers() == 0);

  ExecutionNode* en = registerNode(new ReadAllNode(this, nextId()));

  return addDependency(previous, en);
}

/// @brief create an execution plan from an abstract syntax tree node
ExecutionNode* ExecutionPlan::fromNode(AstNode const* node) {
  TRI_ASSERT(node != nullptr);

  ExecutionNode* en = registerNode(new SingletonNode(this, nextId()));

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

      case NODE_TYPE_K_SHORTEST_PATHS: {
        en = fromNodeKShortestPaths(en, member);
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

      case NODE_TYPE_COLLECT_COUNT: {
        en = fromNodeCollectCount(en, member);
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

      case NODE_TYPE_READ_ALL: {
        en = fromNodeReadAll(en, member);
        break;
      }

      default: {
        // node type not implemented
        en = nullptr;
        break;
      }
    }

    if (en == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "type not handled");
    }
  }

  return en;
}

template <WalkerUniqueness U>
/// @brief find nodes of certain types
void ExecutionPlan::findNodesOfType(::arangodb::containers::SmallVector<ExecutionNode*>& result,
                                    std::initializer_list<ExecutionNode::NodeType> const& types,
                                    bool enterSubqueries) {
  // check if any of the node types is actually present in the plan
  bool haveNodes = std::any_of(types.begin(), types.end(),
                               [this](ExecutionNode::NodeType type) -> bool {
                                 return contains(type);
                               });
  if (haveNodes) {
    // found a node type that is in the plan
    NodeFinder<std::initializer_list<ExecutionNode::NodeType>, U> finder(types, result, enterSubqueries);
    root()->walk(finder);
  }
}

/// @brief find nodes of a certain type
void ExecutionPlan::findNodesOfType(::arangodb::containers::SmallVector<ExecutionNode*>& result,
                                    ExecutionNode::NodeType type, bool enterSubqueries) {
  findNodesOfType<WalkerUniqueness::NonUnique>(result, {type}, enterSubqueries);
}

/// @brief find nodes of certain types
void ExecutionPlan::findNodesOfType(::arangodb::containers::SmallVector<ExecutionNode*>& result,
                                    std::initializer_list<ExecutionNode::NodeType> const& types,
                                    bool enterSubqueries) {
  findNodesOfType<WalkerUniqueness::NonUnique>(result, types, enterSubqueries);
}

/// @brief find nodes of certain types
void ExecutionPlan::findUniqueNodesOfType(
    ::arangodb::containers::SmallVector<ExecutionNode*>& result,
    std::initializer_list<ExecutionNode::NodeType> const& types, bool enterSubqueries) {
  findNodesOfType<WalkerUniqueness::Unique>(result, types, enterSubqueries);
}

/// @brief find all end nodes in a plan
void ExecutionPlan::findEndNodes(::arangodb::containers::SmallVector<ExecutionNode*>& result,
                                 bool enterSubqueries) const {
  EndNodeFinder finder(result, enterSubqueries);
  root()->walk(finder);
}

/// @brief determine and set _varsUsedLater in all nodes
/// as a side effect, count the different types of nodes in the plan
void ExecutionPlan::findVarUsage() {
  if (varUsageComputed()) {
    return;
  }

  // reset all counters
  for (auto& counter : _typeCounts) {
    counter = 0;
  }

  _varSetBy.clear();
  ::VarUsageFinder finder(&_varSetBy);
  root()->walk(finder);

  setVarUsageComputed();
}

/// @brief determine if the above are already set
bool ExecutionPlan::varUsageComputed() const { return _varUsageComputed; }

void ExecutionPlan::planRegisters(ExplainRegisterPlan explainRegisterPlan) {
  _root->planRegisters(nullptr, explainRegisterPlan);
}

/// @brief unlinkNodes, note that this does not delete the removed
/// nodes and that one cannot remove the root node of the plan.
void ExecutionPlan::unlinkNodes(std::unordered_set<ExecutionNode*> const& toRemove) {
  for (auto& node : toRemove) {
    unlinkNode(node);
  }
}

void ExecutionPlan::unlinkNodes(::arangodb::containers::HashSet<ExecutionNode*> const& toRemove) {
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
    // adjust root node. the caller needs to make sure that a new root node gets
    // inserted
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
void ExecutionPlan::replaceNode(ExecutionNode* oldNode, ExecutionNode* newNode) {
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
void ExecutionPlan::insertDependency(ExecutionNode* oldNode, ExecutionNode* newNode) {
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
void ExecutionPlan::insertAfter(ExecutionNode* previous, ExecutionNode* newNode) {
  TRI_ASSERT(newNode != nullptr);
  TRI_ASSERT(previous->id() != newNode->id());
  TRI_ASSERT(newNode->getDependencies().empty());

  std::vector<ExecutionNode*> parents = previous->getParents();  // Intentional copy
  for (ExecutionNode* parent : parents) {
    if (!parent->replaceDependency(previous, newNode)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "Could not replace dependencies of an old node");
    }
  }
  newNode->addDependency(previous);
}

/// @brief insert node directly before current
void ExecutionPlan::insertBefore(ExecutionNode* current, ExecutionNode* newNode) {
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
ExecutionNode* ExecutionPlan::fromSlice(VPackSlice const& slice) {
  if (!slice.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "plan slice is not an object");
  }

  VPackSlice nodes = slice.get("nodes");
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
    // We need to adjust nextId here, otherwise we cannot add new nodes to this
    // plan anymore
    if (_nextId <= ret->id()) {
      _nextId = ExecutionNodeId{ret->id().id() + 1};
    }
    registerNode(ret);

    // we have to count all nodes by their type here, because our caller
    // will set the _varUsageComputed flag to true manually, bypassing the
    // regular counting!
    increaseCounter(ret->getType());

    TRI_ASSERT(ret != nullptr);

    if (ret->getType() == arangodb::aql::ExecutionNode::SUBQUERY) {
      // found a subquery node. now do magick here
      VPackSlice subquery = it.get("subquery");
      // create the subquery nodes from the "subquery" sub-node
      auto subqueryNode = fromSlice(subquery);

      // register the just created subquery
      ExecutionNode::castTo<SubqueryNode*>(ret)->setSubquery(subqueryNode, false);
    }
  }

  // all nodes have been created. now add the dependencies
  for (VPackSlice it : VPackArrayIterator(nodes)) {
    // read the node's own id
    auto thisId =
        ExecutionNodeId{it.get("id").getNumericValue<ExecutionNodeId::BaseType>()};
    auto thisNode = getNodeById(thisId);

    // now re-link the dependencies
    VPackSlice dependencies = it.get("dependencies");
    if (dependencies.isArray()) {
      for (auto const& it2 : VPackArrayIterator(dependencies)) {
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

/// @brief returns true if a plan is so simple that optimizations would
/// probably cost more than simply executing the plan
bool ExecutionPlan::isDeadSimple() const {
  auto current = _root;

  while (current != nullptr) {
    auto const nodeType = current->getType();

    if (nodeType == ExecutionNode::SUBQUERY || nodeType == ExecutionNode::ENUMERATE_COLLECTION ||
        nodeType == ExecutionNode::ENUMERATE_LIST ||
        nodeType == ExecutionNode::TRAVERSAL || nodeType == ExecutionNode::SHORTEST_PATH ||
        nodeType == ExecutionNode::K_SHORTEST_PATHS || nodeType == ExecutionNode::INDEX) {
      // these node types are not simple
      return false;
    }

    current = current->getFirstDependency();
  }

  return true;
}

bool ExecutionPlan::fullCount() const noexcept {
  LimitNode* lastLimitNode = _lastLimitNode == nullptr
                                 ? nullptr
                                 : ExecutionNode::castTo<LimitNode*>(_lastLimitNode);
  return lastLimitNode != nullptr && lastLimitNode->fullCount();
}

/// @brief find all variables that are populated with data from collections
void ExecutionPlan::findCollectionAccessVariables() {
  _ast->variables()->visit([this](Variable* variable) {
    ExecutionNode* node = this->getVarSetBy(variable->id);
    if (node != nullptr) {
      if (node->getType() == ExecutionNode::ENUMERATE_COLLECTION ||
          node->getType() == ExecutionNode::INDEX ||
          node->getType() == ExecutionNode::INSERT ||
          node->getType() == ExecutionNode::UPDATE ||
          node->getType() == ExecutionNode::REPLACE ||
          node->getType() == ExecutionNode::REMOVE) {
        // TODO: maybe add more node types here, e.g. arangosearch nodes
        // also check if it makes sense to do this if the node uses
        // projections
        variable->isDataFromCollection = true;
      }
    }
  });
}

void ExecutionPlan::prepareTraversalOptions() {
  ::arangodb::containers::SmallVector<ExecutionNode*>::allocator_type::arena_type a;
  ::arangodb::containers::SmallVector<ExecutionNode*> nodes{a};
  findNodesOfType(nodes,
                  {arangodb::aql::ExecutionNode::TRAVERSAL,
                   arangodb::aql::ExecutionNode::SHORTEST_PATH,
                   arangodb::aql::ExecutionNode::K_SHORTEST_PATHS},
                  true);
  for (auto& node : nodes) {
    switch (node->getType()) {
      case ExecutionNode::TRAVERSAL:
      case ExecutionNode::SHORTEST_PATH:
      case ExecutionNode::K_SHORTEST_PATHS: {
        auto* graphNode = ExecutionNode::castTo<GraphNode*>(node);
        graphNode->prepareOptions();
      } break;
      default:
        TRI_ASSERT(false);
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
    }
  }
}

AstNode const* ExecutionPlan::resolveVariableAlias(AstNode const* node) const {
  if (node->type == NODE_TYPE_REFERENCE) {
    auto setter = getVarSetBy(static_cast<Variable const*>(node->getData())->id);
    if (setter != nullptr && setter->getType() == ExecutionNode::CALCULATION) {
      auto cn = ExecutionNode::castTo<CalculationNode const*>(setter);
      node = cn->expression()->node();
    }
  }
  return node;
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#include <iostream>

/// @brief show an overview over the plan
struct Shower final : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
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

    LoggerStream logLn{};
    logLn << LogLevel::INFO << Logger::AQL;

    for (int i = 0; i < 2 * indent; i++) {
      logLn << ' ';
    }
    logNode(logLn, *en);

    if (en->getType() == ExecutionNode::SUBQUERY_START) {
      ++indent;
    }
  }

  static LoggerStreamBase& logNode(LoggerStreamBase& log, ExecutionNode const& node) {
    return log << "[" << node.id() << "]" << detailedNodeType(node);
  }

  static std::string detailedNodeType(ExecutionNode const& node) {
    switch (node.getType()) {
      case ExecutionNode::TRAVERSAL:
      case ExecutionNode::SHORTEST_PATH:
      case ExecutionNode::K_SHORTEST_PATHS: {
        auto const& graphNode = *ExecutionNode::castTo<GraphNode const*>(&node);
        auto type = std::string{node.getTypeString()};
        if (graphNode.isUsedAsSatellite()) {
          type += " used as satellite";
        }
        return type;
      }
      case ExecutionNode::INDEX:
      case ExecutionNode::ENUMERATE_COLLECTION:
      case ExecutionNode::UPDATE:
      case ExecutionNode::INSERT:
      case ExecutionNode::REMOVE:
      case ExecutionNode::REPLACE:
      case ExecutionNode::UPSERT: {
        auto const& colAccess =
            *ExecutionNode::castTo<CollectionAccessingNode const*>(&node);
        auto type = std::string{node.getTypeString()};
        type += " (";
        type += colAccess.collection()->name();
        type += ")";
        if (colAccess.isUsedAsSatellite()) {
          type += " used as satellite";
        }
        return type;
      }
      default:
        return {node.getTypeString()};
    }
  }
};

/// @brief show an overview over the plan
void ExecutionPlan::show() const {
  Shower shower;
  _root->walk(shower);
}

#endif
