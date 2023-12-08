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

namespace arangodb::aql::expression_matcher {
// Matches a node of type NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ with the
// left hand side matching LHS and the right hand side matching RHS, using the
// quantifier matching Quantifier
template<Matchable LHS, Matchable RHS, Matchable Quantifier>
struct ArrayEq {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ).apply(node)  //
           |                                                              //
           [&]() {                                                        //
             return lhs                                                   //
                 .apply(node->getMemberUnchecked(0))                      //
                 .addErrorIfError("left hand side match failed");         //
           }                                                              //
           |                                                              //
           [&]() {                                                        //
             return rhs                                                   //
                 .apply(node->getMemberUnchecked(1))                      //
                 .addErrorIfError("right hand side match failed");        //
           }                                                              //
           |                                                              //
           [&]() {                                                        //
             return quantifier                                            //
                 .apply(node->getMemberUnchecked(2))                      //
                 .addErrorIfError("quantifier match failed");             //
           };                                                             //
  }

  LHS lhs;
  RHS rhs;
  Quantifier quantifier;
};

template<Matchable LHS, Matchable RHS, Matchable Quantifier>
auto arrayEq(LHS lhs, RHS rhs, Quantifier quantifier)
    -> ArrayEq<LHS, RHS, Quantifier> {
  return ArrayEq<LHS, RHS, Quantifier>{.lhs = std::move(lhs),
                                       .rhs = std::move(rhs),
                                       .quantifier = std::move(quantifier)};
};

}  // namespace arangodb::aql::expression_matcher
