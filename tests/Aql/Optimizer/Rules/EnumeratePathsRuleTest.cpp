////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#include "gtest/gtest.h"

#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/Expression.h"
#include "Aql/Parser.h"
#include "Aql/Query.h"
#include "Aql/QueryString.h"
#include "Aql/QueryContext.h"
#include "Logger/LogMacros.h"
#include "VelocypackUtils/VelocyPackStringLiteral.h"

#include "Mocks/Servers.h"

#include <vector>
#include <ranges>

using namespace arangodb::aql;
using namespace arangodb::tests;

struct TestContext {
  explicit TestContext(std::string to_parse)
      : server{},
        query{server.createFakeQuery()},
        trx{server.createFakeTransaction()} {
    // TODO: a lot of magic is happening here that is strictly speaking
    // entirely unnecessary.
    auto qs = QueryString(to_parse);
    auto parser = arangodb::aql::Parser(*query, *query->ast(), qs);
    parser.parse();

    // TODO: find out whether this validateAndOptimize step is necessary (or
    // counterproductive) for these tests
    // query->ast()->validateAndOptimize(*trx, {.optimizeNonCacheable = true});
  }

  auto getTopNode() -> AstNode const* {
    return query->ast()->root()->getMemberUnchecked(2)->getMemberUnchecked(0);
  }
  auto getRootNode() -> AstNode const* { return query->ast()->root(); }

  mocks::MockAqlServer server;
  std::shared_ptr<Query> query;
  std::shared_ptr<arangodb::transaction::Methods> trx;
};

TEST(EnumeratePathsRuleTest, matches_type) {
  auto query = TestContext(
      R"=(
       FOR path IN 1..5 OUTBOUND K_PATHS "v/1" TO "w/20" GRAPH "graph"
         FILTER path.vertices[* RETURN CURRENT.colour == "green"] ALL == true
         FILTER path.path[* RETURN CURRENT.shape == "triangle"] ALL == true
         RETURN path)=");

  auto const* node = query.getRootNode();
  (void)node;
  { ASSERT_EQ("black", "white"); }
}
