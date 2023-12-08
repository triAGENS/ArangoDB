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

#include <Aql/Optimizer/ExpressionMatcher/Matchers/NodeType.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchable.h>
#include "Aql/AstNode.h"

namespace arangodb::aql::expression_matcher {

template<Matchable M>
struct NAryOrAll {
  auto apply(AstNode const* node) -> MatchResult {
    // TODO: this must be possible to do better
    return MatchNodeType(NODE_TYPE_OPERATOR_NARY_OR).apply(node)  //
           | [&]() {
               auto mr = MatchResult();

               for (auto i = size_t{0}; i < node->numMembers(); ++i) {
                 mr.combine(matcher.apply(node->getMemberUnchecked(i)));
                 if (mr.isError()) {
                   return mr;
                 }
               }
             };
  }
  M matcher;
};

// Applies the matcher  M and if it succeeds registers the AqlNode* that
// it succeeded on in the MatchResult under the name `name`
template<Matchable M>
struct NAryOrWithOneSubnode {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_OPERATOR_NARY_OR).apply(node)  //
           |
           [&]() {
             if (node->numMembers() != 1) {
               return MatchResult::error("expected precisely 1 member");
             } else {
               return MatchResult::match();
             }
           } |
           [&]() { return matcher.apply(node->getMemberUnchecked(0)); };
  }
  M matcher;
};
template<Matchable M>
auto naryOrWithOneSubnode(M m) -> NAryOrWithOneSubnode<M> {
  return NAryOrWithOneSubnode<M>{.matcher = std::move(m)};
}

// TODO: it should be possible to do the following
template<Matchable... M>
struct NAryOr {
  auto apply(AstNode const* node) -> MatchResult{
      // .. matches precisely the members in order to the given pattern matchers
  };
};

}  // namespace arangodb::aql::expression_matcher
