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

#include <Aql/AstNode.h>
#include <Aql/Optimizer/ExpressionMatcher/MatchResult.h>

namespace arangodb::aql::expression_matcher {

// To implement a matcher, all that is needed is a struct with the method
// apply that takes an `AstNode const*` and returns a MatchResult.
template<typename T>
concept Matchable = std::same_as<decltype(std::declval<T>().apply(
                                     std::declval<AstNode const*>())),
                                 MatchResultSingle>;

template<typename T>
concept MultiMatchable = std::same_as<decltype(std::declval<T>().apply(
                                          std::declval<AstNode const*>())),
                                      MatchResultMulti>;

}  // namespace arangodb::aql::expression_matcher
