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
// Matches a node of type NODE_TYPE_EXPANSION with the
// iterator matching Iterator, the Reference matching Reference, the limit
// expression matching Limit, the filter expression matching Filter and the
// map expression matching Map.
template<Matchable Iterator, Matchable Reference, Matchable Limit,
         Matchable Filter, Matchable Map>
struct Expansion {
  auto apply(AstNode const* node) -> MatchResult {
    return MatchNodeType(NODE_TYPE_EXPANSION).apply(node)     //
           |                                                  //
           [&]() {                                            //
             return iterator                                  //
                 .apply(node->getMemberUnchecked(0))          //
                 .addErrorIfError("iterator match failed");   //
           }                                                  //
           |                                                  //
           [&]() {                                            //
             return reference                                 //
                 .apply(node->getMemberUnchecked(1))          //
                 .addErrorIfError("reference match failed");  //
           }                                                  //
           |                                                  //
           [&]() {                                            //
             return limit                                     //
                 .apply(node->getMemberUnchecked(2))          //
                 .addErrorIfError("limit match failed");      //
           }                                                  //
           |                                                  //
           [&]() {                                            //
             return filter                                    //
                 .apply(node->getMemberUnchecked(3))          //
                 .addErrorIfError("filter match failed");     //
           }                                                  //
           | [&]() {                                          //
               return map                                     //
                   .apply(node->getMemberUnchecked(4))        //
                   .addErrorIfError("map match failed");      //
             };                                               //
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

}  // namespace arangodb::aql::expression_matcher
