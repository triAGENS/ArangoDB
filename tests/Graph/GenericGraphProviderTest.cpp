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

#include "./GraphTestTools.h"
#include "./MockGraph.h"
#include "./MockGraphProvider.h"

#include "Mocks/PreparedResponseConnectionPool.h"

#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/SingleServerProvider.h"

#include <velocypack/velocypack-aliases.h>
#include <unordered_set>

using namespace arangodb;
using namespace arangodb::tests;
using namespace arangodb::tests::graph;
using namespace arangodb::graph;

namespace arangodb {
namespace tests {
namespace generic_graph_provider_test {

static_assert(GTEST_HAS_TYPED_TEST, "We need typed tests for the following:");

// Add more providers here
using TypesToTest =
    ::testing::Types<MockGraphProvider, SingleServerProvider, ClusterProvider>;

template <class ProviderType>
class GraphProviderTest : public ::testing::Test {
 public:
  using Step = typename ProviderType::Step;

 protected:
  arangodb::ResourceMonitor resourceMonitor{};

  // Only used to mock a singleServer
  std::unique_ptr<GraphTestSetup> s{nullptr};
  std::unique_ptr<MockGraphDatabase> singleServer{nullptr};
  std::unique_ptr<mocks::MockServer> server{nullptr};
  std::unique_ptr<arangodb::aql::Query> query{nullptr};

  std::unique_ptr<std::unordered_map<ServerID, aql::EngineId>> clusterEngines{nullptr};

  GraphProviderTest() {}
  ~GraphProviderTest() {}

  auto makeProvider(MockGraph const& graph) -> ProviderType {
    // Setup code for each provider type
    if constexpr (std::is_same_v<ProviderType, MockGraphProvider>) {
      s = std::make_unique<GraphTestSetup>();
      singleServer =
          std::make_unique<MockGraphDatabase>(s->server, "testVocbase");
      singleServer->addGraph(graph);

      // We now have collections "v" and "e"
      query = singleServer->getQuery("RETURN 1", {"v", "e"});

      return MockGraphProvider(graph, *query.get(),
                               MockGraphProvider::LooseEndBehaviour::NEVER);
    }
    if constexpr (std::is_same_v<ProviderType, SingleServerProvider>) {
      s = std::make_unique<GraphTestSetup>();
      singleServer =
          std::make_unique<MockGraphDatabase>(s->server, "testVocbase");
      singleServer->addGraph(graph);

      // We now have collections "v" and "e"
      query = singleServer->getQuery("RETURN 1", {"v", "e"});

      auto edgeIndexHandle = singleServer->getEdgeIndexHandle("e");
      auto tmpVar = singleServer->generateTempVar(query.get());
      auto indexCondition = singleServer->buildOutboundCondition(query.get(), tmpVar);

      std::vector<IndexAccessor> usedIndexes{
          IndexAccessor{edgeIndexHandle, indexCondition, 0}};

      BaseProviderOptions opts(tmpVar, std::move(usedIndexes));
      return SingleServerProvider(*query.get(), std::move(opts), resourceMonitor);
    }
    if constexpr (std::is_same_v<ProviderType, ClusterProvider>) {
      // Prepare the DBServerResponses
      std::vector<arangodb::tests::PreparedRequestResponse> preparedResponses;
      {
        arangodb::tests::mocks::MockDBServer server{};
        graph.prepareServer(server);
        graph.simulateApi(server, preparedResponses);
      }

      server = std::make_unique<mocks::MockCoordinator>(false);
      mocks::MockCoordinator* srv = static_cast<mocks::MockCoordinator*>(server.get());
      graph.prepareServer(*srv);
      auto dbServerEndpoint = srv->registerFakedDBServer("PRMR_0001");
      auto pool = srv->getPool();
      static_cast<arangodb::tests::PreparedResponseConnectionPool*>(pool)
          ->addPreparedResponses(dbServerEndpoint, std::move(preparedResponses));

      {
        auto queryString = arangodb::aql::QueryString("RETURN 1");

        auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
            server->getSystemDatabase());
        query = std::make_unique<arangodb::aql::Query>(ctx, queryString, nullptr);

        query->collections().add("v", AccessMode::Type::READ,
                                 arangodb::aql::Collection::Hint::Collection);
        query->collections().add("e", AccessMode::Type::READ,
                                 arangodb::aql::Collection::Hint::Collection);

        query->prepareQuery(SerializationFormat::SHADOWROWS);
      }

      clusterEngines = std::make_unique<std::unordered_map<ServerID, aql::EngineId>>();
      clusterEngines->emplace("PRMR_0001", 1);

      auto clusterCache =
          std::make_shared<RefactoredClusterTraverserCache>(clusterEngines.get(), resourceMonitor);

      ClusterBaseProviderOptions opts(clusterCache, false);
      return ClusterProvider(*query.get(), std::move(opts), resourceMonitor);
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
};

TYPED_TEST_CASE(GraphProviderTest, TypesToTest);

TYPED_TEST(GraphProviderTest, no_results_if_graph_is_empty) {
  MockGraph empty{};

  auto testee = this->makeProvider(empty);
  std::string startString = "v/0";
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);

  if (start.isLooseEnd()) {
    std::vector<decltype(start)*> looseEnds{};
    looseEnds.emplace_back(&start);
    auto futures = testee.fetch(looseEnds);
    auto steps = futures.get();
  }

  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  EXPECT_EQ(result.size(), 0);
  TraversalStats stats = testee.stealStats();
  EXPECT_EQ(stats.getFiltered(), 0);
  EXPECT_EQ(stats.getHttpRequests(), 0);
  // We have no edges, so nothing scanned in the Index.
  EXPECT_EQ(stats.getScannedIndex(), 0);
}

TYPED_TEST(GraphProviderTest, should_enumerate_a_single_edge) {
  MockGraph g{};
  g.addEdge(0, 1);

  auto testee = this->makeProvider(g);
  std::string startString = "v/0";
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);
  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&result](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  ASSERT_EQ(result.size(), 1);
  auto const& f = result.at(0);
  EXPECT_EQ(f.getVertex().getID().toString(), "v/1");
  EXPECT_EQ(f.getPrevious(), 0);

  {
    TraversalStats stats = testee.stealStats();
    EXPECT_EQ(stats.getFiltered(), 0);
    EXPECT_EQ(stats.getHttpRequests(), 0);
    // We have 1 edge, this shall be counted
    EXPECT_EQ(stats.getScannedIndex(), 1);
  }
  {
    // Make sure stats are reset after we stole them
    // So steal again works, but on empty statistics
    TraversalStats stats = testee.stealStats();
    EXPECT_EQ(stats.getFiltered(), 0);
    EXPECT_EQ(stats.getHttpRequests(), 0);
    EXPECT_EQ(stats.getScannedIndex(), 0);
  }
}

TYPED_TEST(GraphProviderTest, should_enumerate_all_edges) {
  MockGraph g{};
  g.addEdge(0, 1);
  g.addEdge(0, 2);
  g.addEdge(0, 3);
  std::unordered_set<std::string> found{};

  auto testee = this->makeProvider(g);
  std::string startString = "v/0";
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);

  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  ASSERT_EQ(result.size(), 3);
  for (auto const& f : result) {
    // All expand of the same previous
    EXPECT_EQ(f.getPrevious(), 0);
    auto const& v = f.getVertex().getID().toString();
    // We can only range from 1 to 3
    EXPECT_GE(v, "v/1");
    EXPECT_LE(v, "v/3");
    // We need to find each exactly once
    auto const [_, didInsert] = found.emplace(v);
    EXPECT_TRUE(didInsert);
  }

  {
    TraversalStats stats = testee.stealStats();
    EXPECT_EQ(stats.getFiltered(), 0);
    EXPECT_EQ(stats.getHttpRequests(), 0);
    // We have 3 edges, this shall be counted
    EXPECT_EQ(stats.getScannedIndex(), 3);
  }
}

}  // namespace generic_graph_provider_test
}  // namespace tests
}  // namespace arangodb
