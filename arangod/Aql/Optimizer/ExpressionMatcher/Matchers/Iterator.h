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

// Matches a node of type NODE_TYPE_ITERATOR matching the iterator variable
// `variable` and iterating over the node matching Iteratee
template<Matchable Variable, Matchable Iteratee>
struct Iterator {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_ITERATOR).apply(node)     //
           |                                                 //
           [&]() {                                           //
             return variable                                 //
                 .apply(node->getMemberUnchecked(0))         //
                 .addErrorIfError("variable match failed");  //
           }                                                 //
           |                                                 //
           [&]() {                                           //
             return iteratee                                 //
                 .apply(node->getMemberUnchecked(1))         //
                 .addErrorIfError("iteratee match failed");  //
           };                                                //
  }

  Variable variable;
  Iteratee iteratee;
};

template<Matchable Variable, Matchable Iteratee>
auto iterator(Variable variable, Iteratee iteratee)
    -> Iterator<Variable, Iteratee> {
  return Iterator<Variable, Iteratee>{.variable = std::move(variable),
                                      .iteratee = std::move(iteratee)};
}

}  // namespace arangodb::aql::expression_matcher
