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
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <optional>

#include <Aql/AstNode.h>
#include <Aql/Condition.h>
#include <Aql/Variable.h>
#include <Aql/Quantifier.h>

#include "Aql/Optimizer/ExpressionMatcher/MatchResult.h"

namespace arangodb::aql::expression_matcher {

// To implement a matcher, all that is needed is to implement the
// concept Matchable, which is to say a struct with the method
// apply that takes an `AstNode *` and returns a MatchResult.
template<typename T>
concept Matchable = requires(T v, AstNode* node) {
  v.apply(node);
};

// Applies the matcher of type M and if it succeeds registers the AqlNode* that
// it succeeded on in the MatchResult under the name `name`
template<Matchable M>
struct MatchWithName {
  auto apply(AstNode* node) -> MatchResult {
    auto result = matcher.apply(node);

    if (result.isError()) {
      return result;
    }

    return MatchResult::match(name, node);
  }
  std::string name;
  M matcher;
};
// Helper for convenient template argument deduction
template<Matchable M>
auto matchWithName(std::string name, M matcher) -> MatchWithName<M> {
  return MatchWithName<M>{.name = name, .matcher = matcher};
};

// Matches any AstNode
struct Any {
  auto apply(AstNode* node) -> MatchResult { return MatchResult::match(); }
};

// Matches any AstNode that has type `type`
struct MatchNodeType {
  explicit MatchNodeType(AstNodeType type) : type(type) {}
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != type) {
      auto typeName = AstNode::getTypeStringForType(type);
      ADB_PROD_ASSERT(typeName.has_value());
      return MatchResult::error(fmt::format(
          "expected node of type {} ({}), found {} ({})", node->getTypeString(),
          node->type, typeName.value(), type));
    }
    return MatchResult::match();
  }
  AstNodeType type;
};

// Matches AstNodes of type NODE_TYPE_NOP
struct NoOp {
  auto apply(AstNode* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_NOP).apply(node);
  }
};

// Matches AstNodes of type NODE_TYPE_VALUE
struct AnyValue {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_VALUE) {
      return MatchResult::error(fmt::format(
          "Expected node of type `value`, found {}", node->getTypeString()));
    }
    return MatchResult::match();
  }
};

// Matches AstNodes of type NODE_TYPE_VARIABLE
struct AnyVariable {
  auto apply(AstNode* node) -> MatchResult { return MatchResult::match(); }
};

// Matches AstNodes of type NODE_TYPE_VARIABLE with name `name`
struct Variable {
  auto apply(AstNode* node) -> MatchResult {
    auto typeMatch = MatchNodeType(NODE_TYPE_VARIABLE).apply(node);
    if (typeMatch.isError()) {
      return typeMatch;
    }

    auto var = static_cast<::arangodb::aql::Variable const*>(node->getData());
    if (var->name != name) {
      return MatchResult::error(fmt::format(
          "Expected access to variable `{}`, but found {}", name, var->name));
    }
    return MatchResult::match();
  }

  std::string name;
};
// helper for naming consistency
auto variable(std::string name) -> Variable;

// Matches a node of type NODE_TYPE_QUANTIFIER with quantifier `which`
struct Quantifier {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_QUANTIFIER) {
      return MatchResult::error(fmt::format(
          "Expected node of type `quantifier`, found type `{}` instead",
          node->getTypeString()));
    }

    auto maybeType = ::arangodb::aql::Quantifier::getType(node);
    TRI_ASSERT(maybeType.has_value());

    auto type = maybeType.value();
    if (type != which) {
      return MatchResult::error(
          fmt::format("Expected quantifier of type `{}` but found type `{}`",
                      ::arangodb::aql::Quantifier::stringify(which),
                      ::arangodb::aql::Quantifier::stringify(type)));
    }

    return MatchResult::match();
  }

  ::arangodb::aql::Quantifier::Type which;
};

// Matches a node of type NODE_TYPE_REFERENCE, referencing `name`
struct Reference {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_REFERENCE) {
      return MatchResult::error(fmt::format(
          "Expected node type `reference`, found {}", node->getTypeString()));
    }

    auto variable =
        static_cast<::arangodb::aql::Variable const*>(node->getData());
    if (variable->name != name) {
      return MatchResult::error(
          fmt::format("Expected variable `{}` to be referenced, but found `{}`",
                      name, variable->name));
    }
    return MatchResult::match();
  }

  std::string name;
};

// Matches a node of type NODE_TYPE_ATTRIBUTE_ACCESS matching the reference
// `reference` and accessing the attribute `attribute`
template<Matchable Reference>
struct AttributeAccess {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
      return MatchResult::error(
          fmt::format("Expected node type `attribute access`, found {}",
                      node->getTypeString()));
    }

    auto referenceMatch = reference.apply(node->getMemberUnchecked(0));
    if (referenceMatch.isError()) {
      return MatchResult::error(std::move(referenceMatch),
                                "reference match failed");
    }

    auto accessedAttribute = node->getString();
    if (not attributes.contains(accessedAttribute)) {
      return MatchResult::error(fmt::format(
          "expecting to attribute access any of [{}], but found `{}`",
          fmt::join(attributes, ", "), accessedAttribute));
    }
    return MatchResult::match();
  }

  Reference reference;
  std::unordered_set<std::string> attributes;
};

// Helper for template parameter deduction
template<Matchable Reference>
auto attributeAccess(Reference reference,
                     std::unordered_set<std::string> attributes)
    -> AttributeAccess<Reference> {
  return AttributeAccess<Reference>{.reference = reference,
                                    .attributes = attributes};
}

// Matches a node of type NODE_TYPE_ITERATOR matching the iterator variable
// `variable` and iterating over the node matching Iteratee
template<Matchable Variable, Matchable Iteratee>
struct Iterator {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_ITERATOR) {
      return MatchResult::error(fmt::format(
          "Expected node type `iterator`, found {}", node->getTypeString()));
    }

    auto variableMatch = variable.apply(node->getMemberUnchecked(0));
    if (variableMatch.isError()) {
      return MatchResult::error(std::move(variableMatch),
                                fmt::format("variable match failed"));
    }

    auto iterateeMatch = iteratee.apply(node->getMemberUnchecked(1));
    if (iterateeMatch.isError()) {
      return MatchResult::error(std::move(iterateeMatch),
                                fmt::format("iteratee match failed"));
    }

    return MatchResult(std::move(variableMatch), std::move(iterateeMatch));
  }

  Variable variable;
  Iteratee iteratee;
};

template<Matchable Variable, Matchable Iteratee>
auto iterator(Variable variable, Iteratee iteratee)
    -> Iterator<Variable, Iteratee> {
  return Iterator<Variable, Iteratee>{.variable = variable,
                                      .iteratee = iteratee};
}

// Matches a node of type NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ with the
// left hand side matching LHS and the right hand side matching RHS, using the
// quantifier matching Quantifier
template<Matchable LHS, Matchable RHS, Matchable Quantifier>
struct ArrayEq {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ) {
      return MatchResult::error(
          fmt::format("Expected node of type `binary eq all`, found {}",
                      node->getTypeString()));
    }

    auto lhsMatch = lhs.apply(node->getMemberUnchecked(0));
    if (lhsMatch.isError()) {
      return MatchResult::error(std::move(lhsMatch),
                                fmt::format("left hand side match failed"));
    }

    auto rhsMatch = rhs.apply(node->getMemberUnchecked(1));
    if (rhsMatch.isError()) {
      return MatchResult::error(std::move(rhsMatch),
                                fmt::format("right hand side match failed"));
    }

    auto quantifierMatch = quantifier.apply(node->getMemberUnchecked(2));
    if (quantifierMatch.isError()) {
      return MatchResult::error(std::move(quantifierMatch),
                                fmt::format("quantifier match failed"));
    }

    return MatchResult(std::move(lhsMatch), std::move(rhsMatch));
  }

  LHS lhs;
  RHS rhs;
  Quantifier quantifier;
};

template<Matchable LHS, Matchable RHS, Matchable Quantifier>
auto arrayEq(LHS lhs, RHS rhs, Quantifier quantifier)
    -> ArrayEq<LHS, RHS, Quantifier> {
  return ArrayEq<LHS, RHS, Quantifier>{
      .lhs = lhs, .rhs = rhs, .quantifier = quantifier};
};

// Matches a node of type NODE_TYPE_EXPANSION with the
// iterator matching Iterator, the Reference matching Reference, the limit
// expression matching Limit, the filter expression matching Filter and the map
// expression matching Map.
template<Matchable Iterator, Matchable Reference, Matchable Limit,
         Matchable Filter, Matchable Map>
struct Expansion {
  auto apply(AstNode* node) -> MatchResult {
    if (node->type != NODE_TYPE_EXPANSION) {
      return MatchResult::error(
          fmt::format("Expected node of type `expansion`, found {}",
                      node->getTypeString()));
    }

    auto iteratorMatch = iterator.apply(node->getMemberUnchecked(0));
    if (iteratorMatch.isError()) {
      return MatchResult::error(std::move(iteratorMatch),
                                fmt::format("Iterator match failed"));
    }

    auto referenceMatch = reference.apply(node->getMemberUnchecked(1));
    if (referenceMatch.isError()) {
      return MatchResult::error(std::move(referenceMatch),
                                fmt::format("Reference match failed"));
    }

    auto limitMatch = limit.apply(node->getMemberUnchecked(3));
    if (limitMatch.isError()) {
      return MatchResult::error(std::move(limitMatch),
                                fmt::format("Limit match failed"));
    }

    auto filterMatch = filter.apply(node->getMemberUnchecked(2));
    if (filterMatch.isError()) {
      return MatchResult::error(std::move(filterMatch),
                                fmt::format("Filter match failed"));
    }

    auto mapMatch = map.apply(node->getMemberUnchecked(4));
    if (mapMatch.isError()) {
      return MatchResult::error(std::move(mapMatch),
                                fmt::format("Map match failed"));
    }

    return iteratorMatch  //
        .combine(std::move(referenceMatch))
        .combine(std::move(limitMatch))
        .combine(std::move(filterMatch))
        .combine(std::move(mapMatch));
  }

  Iterator iterator;
  Reference reference;
  Limit limit;
  Filter filter;
  Map map;
};

template<Matchable Iterator, Matchable Reference, Matchable Limit,
         Matchable Filter, Matchable Map>
auto expansion(Iterator iterator, Reference reference, Limit limit,
               Filter filter, Map map)
    -> Expansion<Iterator, Reference, Limit, Filter, Map> {
  return Expansion<Iterator, Reference, Limit, Filter, Map>{
      .iterator = std::move(iterator),
      .reference = std::move(reference),
      .limit = std::move(limit),
      .filter = std::move(filter),
      .map = std::move(map)};
}

template<Matchable Subnode>
struct NaryOrWithOneSubNode {
  auto apply(AstNode* node) -> MatchResult {
    auto nodeTypeMatch = MatchNodeType(NODE_TYPE_OPERATOR_NARY_OR).apply(node);
    if (nodeTypeMatch.isError()) {
      return nodeTypeMatch;
    }

    if (node->numMembers() != 1) {
      return MatchResult::error(fmt::format(
          "Expected n-ary or with only 1 subnode, found {} subnodes",
          node->numMembers()));
    }

    auto subnodeMatch = subnode.apply(node->getMemberUnchecked(0));
    if (subnodeMatch.isError()) {
      return MatchResult::error(std::move(subnodeMatch),
                                "Subnode match failed");
    }

    return MatchResult::match();
  }

  Subnode subnode;
};

template<Matchable SubNode>
auto naryOrWithOneSubNode(SubNode subnode) -> NaryOrWithOneSubNode<SubNode> {
  return NaryOrWithOneSubNode<SubNode>{.subnode = subnode};
}

template<Matchable SubMatch>
struct ForAllSubNodes {
  auto apply(AstNode* node) -> MatchResult {
    auto num = node->numMembers();

    auto result = MatchResult::match();

    for (auto i = size_t{0}; i < num; ++i) {
      auto match = submatch.apply(node->getMemberUnchecked(i));
      if (match.isError()) {
        return MatchResult::error(std::move(match),
                                  "Submatch failed for subnode");
      }
      result.combine(std::move(match));
    }

    return result;
  }

  SubMatch submatch;
};

template<Matchable SubMatch>
auto forAllSubNodes(SubMatch submatch) -> ForAllSubNodes<SubMatch> {
  return ForAllSubNodes<SubMatch>{.submatch = submatch};
};
}  // namespace arangodb::aql::expression_matcher
