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

#include <Aql/Quantifier.h>

#include <Aql/Optimizer/ExpressionMatcher/Matchable.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NodeType.h>

#include <Assertions/Assert.h>

namespace arangodb::aql::expression_matcher {

// Matches a node of type NODE_TYPE_QUANTIFIER with quantifier `which`
struct Quantifier {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_QUANTIFIER).apply(node)  //
           | [&]() {
               auto maybeType = ::arangodb::aql::Quantifier::getType(node);
               TRI_ASSERT(maybeType.has_value());

               auto type = maybeType.value();
               if (type != which) {
                 return MatchResult::error(fmt::format(
                     "Expected quantifier of type `{}` but found type `{}`",
                     ::arangodb::aql::Quantifier::stringify(which),
                     ::arangodb::aql::Quantifier::stringify(type)));
               }
               return MatchResult::match();
             };
  }

  ::arangodb::aql::Quantifier::Type which;
};

auto quantifier(::arangodb::aql::Quantifier::Type which) -> Quantifier;

}  // namespace arangodb::aql::expression_matcher
