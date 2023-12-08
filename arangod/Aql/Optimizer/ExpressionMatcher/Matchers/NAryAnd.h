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

namespace arangodb::aql::expression_matcher {

template<Matchable M>
struct NAryAndAll {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_OPERATOR_NARY_AND).apply(node)  //
           | [&]() {
               for (auto i = size_t{0}; i < node->numMembers(); ++i) {
                 auto snr = matcher.apply(node->getMemberUnchecked(i));
               }
               return MatchResult();
             };
  }
  M matcher;
};

template<Matchable M>
auto naryAndAll(M m) -> NAryAndAll<M> {
  return NAryAndAll<M>{.matcher = std::move(m)};
}

}  // namespace arangodb::aql::expression_matcher
