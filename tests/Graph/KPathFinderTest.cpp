////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "GraphTestTools.h"

#include "Basics/StringUtils.h"
#include "Graph/KPathFinder.h"
#include "Graph/ShortestPathOptions.h"

using namespace arangodb::velocypack;

namespace arangodb {
namespace tests {
namespace graph {


class KPathFinderTest : public ::testing::Test {
 protected:
  GraphTestSetup s;
  MockGraphDatabase gdb;

  std::unique_ptr<arangodb::aql::Query> query;
  std::unique_ptr<arangodb::graph::ShortestPathOptions> spo;

  std::unique_ptr<KPathFinder> finder;

  KPathFinderTest() : gdb(s.server, "testVocbase") {
    gdb.addVertexCollection("v", 100);
    gdb.addEdgeCollection(
        "e", "v",
        {{1, 2},   {2, 3},   {3, 4},   {5, 4},   {6, 5},   {7, 6},   {8, 7},
         {1, 10},  {10, 11}, {11, 12}, {12, 4},  {12, 5},  {21, 22}, {22, 23},
         {23, 24}, {24, 25}, {21, 26}, {26, 27}, {27, 28}, {28, 25}, {30, 31},
         {31, 32}, {32, 33}, {33, 34}, {34, 35}, {32, 30}, {33, 35}, {40, 41},
         {41, 42}, {41, 43}, {42, 44}, {43, 44}, {44, 45}, {45, 46}, {46, 47},
         {48, 47}, {49, 47}, {50, 47}, {48, 46}, {50, 46}, {50, 47}, {48, 46},
         {50, 46}, {40, 60}, {60, 61}, {61, 62}, {62, 63}, {63, 64}, {64, 47},
         {70, 71}, {70, 71}, {70, 71}});

    query = gdb.getQuery("RETURN 1", std::vector<std::string>{"v", "e"});
    spo = gdb.getShortestPathOptions(query.get());

    finder = std::make_unique<KPathFinder>(*spo);
  }

  auto vId(size_t nr) -> std::string {
      return "v/" + basics::StringUtils::itoa(nr);
  }
};

TEST_F(KPathFinderTest, no_path_exists) {
    VPackBuilder result;
    // No path between those
    auto source = vId(91);
    auto target = vId(99);
    finder->reset(StringRef(source), StringRef(target));

    EXPECT_TRUE(finder->hasMore());
    {
      auto hasPath = finder->getNextPath(result);
      EXPECT_FALSE(hasPath);
      EXPECT_TRUE(result.isEmpty());
      EXPECT_FALSE(finder->hasMore());
    }
    
    {
      // Try again to make sure we stay at non-existing
      auto hasPath = finder->getNextPath(result);
      EXPECT_FALSE(hasPath);
      EXPECT_TRUE(result.isEmpty());
      EXPECT_FALSE(finder->hasMore());
    }
}
}
}
}