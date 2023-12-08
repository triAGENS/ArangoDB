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

#include <Assertions/ProdAssert.h>

#include <fmt/core.h>

namespace arangodb::aql::expression_matcher {

// Matches any AstNode that has type `type`
struct MatchNodeType {
  MatchNodeType(AstNodeType type) : type(type) {}
  auto apply(AstNode const* node) -> MatchResult {
    if (node->type != type) {
      auto typeName = AstNode::getTypeStringForType(type);
      ADB_PROD_ASSERT(typeName.has_value());
      return MatchResult::error(
          fmt::format("expected node of type {}, found {}",
                      node->getTypeString(), typeName.value()));
    }
    return MatchResult::match();
  }
  AstNodeType type;
};

}  // namespace arangodb::aql::expression_matcher
