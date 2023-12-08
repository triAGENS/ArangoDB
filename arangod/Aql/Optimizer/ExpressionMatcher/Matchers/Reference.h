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

#include <Aql/Variable.h>

#include <Aql/Optimizer/ExpressionMatcher/Matchable.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NodeType.h>

namespace arangodb::aql::expression_matcher {
// Matches a node of type NODE_TYPE_REFERENCE, referencing `name`
struct Reference {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_REFERENCE).apply(node)  //
           |
           [&]() {
             auto variable =
                 static_cast<::arangodb::aql::Variable const*>(node->getData());
             if (variable->name != name) {
               return MatchResult::error(fmt::format(
                   "Expected variable `{}` to be referenced, but found `{}`",
                   name, variable->name));
             }
             return MatchResult::match();
           };
  }

  std::string name;
};

auto reference(std::string name) -> Reference;

}  // namespace arangodb::aql::expression_matcher
