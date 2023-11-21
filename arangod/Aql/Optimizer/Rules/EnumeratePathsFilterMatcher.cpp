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
////////////////////////////////////////////////////////////////////////////////
#include "EnumeratePathsFilterMatcher.h"
#include <optional>
#include <ostream>
#include <variant>

#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/Expression.h"
#include "Aql/EnumeratePathsNode.h"
#include "Aql/Quantifier.h"
#include "Aql/Optimizer/ExpressionMatcher/ExpressionMatcher.h"

#include "Basics/ErrorT.h"
#include "Basics/overload.h"

using namespace arangodb::aql::expression_matcher;
using EN = arangodb::aql::ExecutionNode;

namespace {}

namespace arangodb::aql {

EnumeratePathsFilterMatcher::EnumeratePathsFilterMatcher(ExecutionPlan* plan)
    : _plan(plan), _condition(std::make_unique<Condition>(plan->getAst())) {}

auto EnumeratePathsFilterMatcher::before(ExecutionNode* node) -> bool {
  if (!_condition->isEmpty() && !node->isDeterministic()) {
    // found a FILTER and something that is not deterministic is not safe to
    // optimize

    _filterVariables.clear();
    _condition = std::make_unique<Condition>(_plan->getAst());
    return true;
  }

  switch (node->getType()) {
    case EN::ENUMERATE_LIST:
    case EN::COLLECT:
    case EN::SCATTER:
    case EN::DISTRIBUTE:
    case EN::GATHER:
    case EN::REMOTE:
    case EN::SUBQUERY:
    case EN::INDEX:
    case EN::JOIN:
    case EN::RETURN:
    case EN::SORT:
    case EN::ENUMERATE_COLLECTION:
    case EN::LIMIT:
    case EN::SHORTEST_PATH:
    case EN::TRAVERSAL:
    case EN::ENUMERATE_IRESEARCH_VIEW:
    case EN::WINDOW: {
      // the above node types can safely be ignored for the purposes
      // of this optimizer
    } break;

    case EN::INSERT:
    case EN::REMOVE:
    case EN::REPLACE:
    case EN::UPDATE:
    case EN::UPSERT: {
      // modification invalidates the filter expression
      _condition = std::make_unique<Condition>(_plan->getAst());
      _filterVariables.clear();
    } break;

    case EN::SINGLETON:
    case EN::NORESULTS: {
      return true;
    } break;

    case EN::FILTER: {
      // A FILTER node just tests a variable for truth(iness?);
      // the condition that is used to filter is calculated in an
      // CALCULATION_NODE before
      // So below we pick up the calculation for the filter (keep
      // in mind that the plan is traversed bottom to top.
      _filterVariables.emplace(
          ExecutionNode::castTo<FilterNode const*>(node)->inVariable()->id);
    } break;

    case EN::CALCULATION: {
      auto calcNode = ExecutionNode::castTo<CalculationNode const*>(node);
      Variable const* outVar = calcNode->outVariable();
      if (_filterVariables.find(outVar->id) != _filterVariables.end()) {
        // This calculationNode is directly part of a filter condition
        // So we have to iterate through it.
        _condition->andCombine(calcNode->expression()->node());
      }
    } break;

    case EN::ENUMERATE_PATHS: {
      if (_condition->isEmpty()) {
        // No condition, no optimize
        break;
      }

      _condition->normalize();

      auto pathsNode = ExecutionNode::castTo<EnumeratePathsNode*>(node);
      TRI_ASSERT(pathsNode != nullptr);

      // ENUMERATE_PATHS has one output variable `pathVar`,
      // the conditions that are supported are ones that
      // access p.vertices[*] or p.edges[*], or both
      auto pathVar = &pathsNode->pathOutVariable();
      //      auto const& varsValidInPathEnumeration =
      //      pathsNode->getVarsValid();

      // TODO: due to lack of time for design right now, I split this
      // match into two matches. Here's (an approximation of) the match I would
      // like to write. When this is integrated into releaseable code, the
      // design should be finished to a sufficient degree.
      //
      // For this application the result of the match should be a vector
      // of matches that can then be used to rewrite the query
#if 0
      auto matcher = naryOrWithOneSubNode(forAllSubNodes(arrayEq(         //
          expansion(                                                      //
              iterator(AnyVariable{},                                     //
                       attributeAccess(Reference{.name = pathVar->name},  //
                                       {"edges", "vertices"})),           //
              matchWithName("variable", Any{}),                           //
              NoOp{},                                                     //
              NoOp{},                                                     //
              matchWithName("map", Any{})),                               //
          matchWithName("rhs_value", AnyValue{}),                         //
          expression_matcher::Quantifier{                                 //
              .which = ::arangodb::aql::Quantifier::Type::kAll})));       //
#endif

      auto topMatcher =
          naryOrWithOneSubNode(MatchNodeType(NODE_TYPE_OPERATOR_NARY_AND));
      auto topResult = topMatcher.apply(_condition->root());

      if (topResult.isError()) {
        // No optimisation with reason topResult.errors()
        LOG_DEVEL << "enumerate paths filter optimiser failed: "
                  << toString(topResult);
        return false;
      }

      // Every condition has to be of the form
      // pathVariable.vertices[* ...] ALL == $literal_value
      // pathVariable.edges[* ...] ALL == $literal_value

      auto const* andNode = _condition->root()->getMemberUnchecked(0);
      auto const num = andNode->numMembers();

      auto vertexMatcher = arrayEq(                                       //
          expansion(                                                      //
              iterator(matchWithName("current", AnyVariable{}),           //
                       attributeAccess(Reference{.name = pathVar->name},  //
                                       {"vertices"})),                    //
              Any{},                                                      //
              NoOp{},                                                     //
              NoOp{},                                                     //
              matchWithName("map", Any{})),                               //
          matchWithName("rhs_value", AnyValue{}),                         //
          expression_matcher::Quantifier{
              .which = ::arangodb::aql::Quantifier::Type::kAll});  //

      auto edgeMatcher = arrayEq(                                         //
          expansion(                                                      //
              iterator(matchWithName("current", AnyVariable{}),           //
                       attributeAccess(Reference{.name = pathVar->name},  //
                                       {"edges"})),                       //
              Any{},                                                      //
              NoOp{},                                                     //
              NoOp{},                                                     //
              matchWithName("map", Any{})),                               //
          matchWithName("rhs_value", AnyValue{}),                         //
          expression_matcher::Quantifier{
              .which = ::arangodb::aql::Quantifier::Type::kAll});  //

      auto* ast = _plan->getAst();

      auto applyM = [&pathsNode, &ast](auto&& m, AstNode* node) -> AstNode* {
        auto result = m.apply(node);

        if (!result.isError()) {
          // here `map` is $expr in `path.vertices[* $expr] == $rhsValue`, and
          // it is assumed that the only variable reference in $expr is the
          // variable filled by the iterator of the expansion.
          //
          // This variable reference is then replaced with the temporary
          // variable from the EnumeratePathsNode, as this is in turn used by
          // the PathValidator to evaluate the condition on the vertices.
          // (yes, really)
          auto* map = result.matches().at("map");
          auto* tmpVar = pathsNode->getTemporaryRefNode();
          map =
              Ast::traverseAndModify(map, [&tmpVar](AstNode* node) -> AstNode* {
                if (node->type == NODE_TYPE_REFERENCE) {
                  return tmpVar;
                }
                return node;
              });

          // as per the pattern match above the right hand side is just a
          // value
          auto const* rhsValue = result.matches().at("rhs_value");

          // build a condition of the form
          // (lambda (vertex)
          //   (== (map vertex) rhsValue))
          // this condition has to hold for every vertex, and is registered as
          // such with the path enumeration.
          auto condition = ast->createNodeBinaryOperator(
              NODE_TYPE_OPERATOR_BINARY_EQ, map, rhsValue);

          return condition;
        }

        return nullptr;
      };

      for (auto i = std::size_t{0}; i < num; ++i) {
        {
          auto condition =
              applyM(vertexMatcher, andNode->getMemberUnchecked(i));

          if (condition != nullptr) {
            pathsNode->registerGlobalVertexCondition(condition);
          }
        }

        {
          auto condition = applyM(edgeMatcher, andNode->getMemberUnchecked(i));

          if (condition != nullptr) {
            pathsNode->registerGlobalEdgeCondition(condition);
          }
        }
      }
    } break;
    default: {
      // TODO: should this maybe just prevent the optimiser rule to fire
      // instead of crashing?
      ADB_PROD_ASSERT(false)
          << fmt::format("Unsupported node type {}.", node->getTypeString());
    }
  }
  return false;
}

auto EnumeratePathsFilterMatcher::enterSubquery(ExecutionNode* node1,
                                                ExecutionNode* node2) -> bool {
  return false;
}

}  // namespace arangodb::aql
