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

#include <Aql/Optimizer/ExpressionMatcher/Matchable.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NodeType.h>

#include <unordered_set>

namespace arangodb::aql::expression_matcher {

// Matches a node of type NODE_TYPE_ATTRIBUTE_ACCESS matching the reference
// `reference` and accessing the attribute `attribute`
template<Matchable Reference>
struct AttributeAccess {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_ATTRIBUTE_ACCESS).apply(node)              //
           | [&]() { return reference.apply(node->getMemberUnchecked(0)); }   //
           | [&]() {                                                          //
               auto accessedAttribute = node->getString();                    //
               if (not attributes.contains(accessedAttribute)) {              //
                 return MatchResult::error(fmt::format(                       //
                     "expecting to attribute access any of [{}], but found "  //
                     "`{}`",                                                  //
                     fmt::join(attributes, ", "), accessedAttribute));        //
               }                                                              //
               return MatchResult::match();                                   //
             };                                                               //
  }
  Reference reference;
  std::unordered_set<std::string> attributes;
};

// Helper for template parameter deduction
template<Matchable Reference>
auto attributeAccess(Reference reference,
                     std::unordered_set<std::string> attributes)
    -> AttributeAccess<Reference> {
  return AttributeAccess<Reference>{.reference = std::move(reference),
                                    .attributes = std::move(attributes)};
}

}  // namespace arangodb::aql::expression_matcher
