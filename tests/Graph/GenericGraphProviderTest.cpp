////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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

#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Mocks/PreparedResponseConnectionPool.h"
#include "Mocks/MockQuery.h"

#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/SingleServerProvider.h"
#include "Graph/Steps/SingleServerProviderStep.h"
#include "Graph/TraverserOptions.h"

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
    ::testing::Types<MockGraphProvider,
                     SingleServerProvider<SingleServerProviderStep>,
                     ClusterProvider<ClusterProviderStep>>;

template<class ProviderType>
class GraphProviderTest : public ::testing::Test {
 public:
  using Step = typename ProviderType::Step;

 protected:
  // Only used to mock a singleServer
  std::unique_ptr<GraphTestSetup> s{nullptr};
  std::unique_ptr<MockGraphDatabase> singleServer{nullptr};
  std::unique_ptr<mocks::MockServer> server{nullptr};
  std::shared_ptr<arangodb::aql::Query> query{nullptr};
  std::unique_ptr<std::unordered_map<ServerID, aql::EngineId>> clusterEngines{
      nullptr};
  std::unique_ptr<arangodb::transaction::Methods> _trx{};

  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor resourceMonitor{global};
  arangodb::aql::AqlFunctionsInternalCache _functionsCache{};
  std::unique_ptr<arangodb::aql::FixedVarExpressionContext> _expressionContext;
  ResourceUsageAllocator<MonitoredCollectionToShardMap, ResourceMonitor> alloc =
      {resourceMonitor};
  MonitoredCollectionToShardMap _emptyShardMap{alloc};
  aql::Projections _vertexProjections{};
  aql::Projections _edgeProjections{};

  GraphProviderTest() {}
  ~GraphProviderTest() {}

  auto makeProvider(
      MockGraph const& graph,
      std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> const&
          expectedVerticesEdgesBundleToFetch) -> ProviderType {
    // Setup code for each provider type
    if constexpr (std::is_same_v<ProviderType, MockGraphProvider>) {
      s = std::make_unique<GraphTestSetup>();
      singleServer =
          std::make_unique<MockGraphDatabase>(s->server, "testVocbase");
      singleServer->addGraph(graph);

      // We now have collections "v" and "e"
      query = singleServer->getQuery("RETURN 1", {"v", "e"});

      return MockGraphProvider(
          *query.get(),
          MockGraphProviderOptions{graph,
                                   MockGraphProvider::LooseEndBehaviour::NEVER},
          resourceMonitor);
    }
    if constexpr (std::is_same_v<ProviderType, SingleServerProvider<
                                                   SingleServerProviderStep>>) {
      s = std::make_unique<GraphTestSetup>();
      singleServer =
          std::make_unique<MockGraphDatabase>(s->server, "testVocbase");
      singleServer->addGraph(graph);

      // We now have collections "v" and "e"
      query = singleServer->getQuery("RETURN 1", {"v", "e"});
      _trx = std::make_unique<arangodb::transaction::Methods>(
          query->newTrxContext());

      auto edgeIndexHandle = singleServer->getEdgeIndexHandle("e");
      auto tmpVar = singleServer->generateTempVar(query.get());
      auto indexCondition =
          singleServer->buildOutboundCondition(query.get(), tmpVar);

      std::vector<IndexAccessor> usedIndexes{};
      usedIndexes.emplace_back(IndexAccessor{edgeIndexHandle, indexCondition, 0,
                                             nullptr, std::nullopt, 0,
                                             TRI_EDGE_OUT});

      _expressionContext =
          std::make_unique<arangodb::aql::FixedVarExpressionContext>(
              *_trx.get(), *query, _functionsCache);
      std::vector<Variable const*> vars;
      std::vector<RegisterId const*> regs;

      SingleServerBaseProviderOptions opts(
          tmpVar,
          std::make_pair(
              std::move(usedIndexes),
              std::unordered_map<uint64_t, std::vector<IndexAccessor>>{}),
          *_expressionContext.get(), {}, _emptyShardMap, _vertexProjections,
          _edgeProjections, /*produceVertices*/ true, /*useCache*/ true);
      return SingleServerProvider<SingleServerProviderStep>(
          *query.get(), std::move(opts), resourceMonitor);
    }
    if constexpr (std::is_same_v<ProviderType,
                                 ClusterProvider<ClusterProviderStep>>) {
      // Prepare the DBServerResponses
      std::vector<arangodb::tests::PreparedRequestResponse> preparedResponses;
      uint64_t engineId = 0;
      {
        arangodb::tests::mocks::MockDBServer server{"PRMR_0001", true, true};
        graph.prepareServer(server);

        auto queryString =
            arangodb::aql::QueryString(std::string_view("RETURN 1"));

        auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
            server.getSystemDatabase(), transaction::OperationOriginTestCase{});
        auto fakeQuery = std::make_shared<MockQuery>(ctx, queryString);
        try {
          fakeQuery->collections().add("s9880", AccessMode::Type::READ,
                                       arangodb::aql::Collection::Hint::Shard);
        } catch (...) {
        }
        waitForAsync(fakeQuery->prepareQuery());
        auto ast = fakeQuery->ast();
        auto tmpVar = ast->variables()->createTemporaryVariable();
        auto tmpVarRef = ast->createNodeReference(tmpVar);
        auto tmpIdNode = ast->createNodeValueString("", 0);

        ShortestPathOptions opts{*fakeQuery};
        opts.setVariable(tmpVar);

        auto const* access = ast->createNodeAttributeAccess(
            tmpVarRef, StaticStrings::FromString);
        auto const* cond = ast->createNodeBinaryOperator(
            NODE_TYPE_OPERATOR_BINARY_EQ, access, tmpIdNode);
        auto fromCondition =
            ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
        fromCondition->addMember(cond);
        opts.addLookupInfo(fakeQuery->plan(), "s9880",
                           StaticStrings::FromString, fromCondition,
                           /*onlyEdgeIndexes*/ false, TRI_EDGE_OUT);

        auto const* revAccess =
            ast->createNodeAttributeAccess(tmpVarRef, StaticStrings::ToString);
        auto const* revCond = ast->createNodeBinaryOperator(
            NODE_TYPE_OPERATOR_BINARY_EQ, revAccess, tmpIdNode);
        auto toCondition =
            ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
        toCondition->addMember(revCond);
        opts.addReverseLookupInfo(fakeQuery->plan(), "s9880",
                                  StaticStrings::ToString, toCondition,
                                  /*onlyEdgeIndexes*/ false, TRI_EDGE_IN);

        std::tie(preparedResponses, engineId) =
            graph.simulateApi(server, expectedVerticesEdgesBundleToFetch, opts);
        // Note: Please don't remove for debugging purpose.
#if 0
        for (auto const& resp : preparedResponses) {
          auto payload = resp.generateResponse()->copyPayload();
          VPackSlice husti(payload->data());
          LOG_DEVEL << husti.toJson();
        }
#endif
      }

      server =
          std::make_unique<mocks::MockCoordinator>("CRDN_0001", true, false);
      mocks::MockCoordinator* srv =
          static_cast<mocks::MockCoordinator*>(server.get());
      graph.prepareServer(*srv);
      auto dbServerEndpoint = srv->registerFakedDBServer("PRMR_0001");
      auto pool = srv->getPool();
      static_cast<arangodb::tests::PreparedResponseConnectionPool*>(pool)
          ->addPreparedResponses(dbServerEndpoint,
                                 std::move(preparedResponses));

      {
        auto queryString =
            arangodb::aql::QueryString(std::string_view("RETURN 1"));

        auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
            server->getSystemDatabase(),
            transaction::OperationOriginTestCase{});
        query = arangodb::aql::Query::create(ctx, queryString, nullptr);

        query->collections().add("v", AccessMode::Type::READ,
                                 arangodb::aql::Collection::Hint::Collection);
        query->collections().add("e", AccessMode::Type::READ,
                                 arangodb::aql::Collection::Hint::Collection);

        waitForAsync(query->prepareQuery());
      }

      clusterEngines =
          std::make_unique<std::unordered_map<ServerID, aql::EngineId>>();
      clusterEngines->emplace("PRMR_0001", engineId);

      auto clusterCache =
          std::make_shared<RefactoredClusterTraverserCache>(resourceMonitor);

      ClusterBaseProviderOptions opts(clusterCache, clusterEngines.get(), false,
                                      true);
      return ClusterProvider<ClusterProviderStep>(*query.get(), std::move(opts),
                                                  resourceMonitor);
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
};

TYPED_TEST_CASE(GraphProviderTest, TypesToTest);

TYPED_TEST(GraphProviderTest, no_results_if_graph_is_empty) {
  MockGraph empty{};

  std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> const&
      expectedVerticesEdgesBundleToFetch = {{0, {}}};
  // IMPORTANT Note: As soon as we're going to include vertices here with a
  // depth > 1, we have to adjust the API of MockGraph::simulateApi. As
  // This value is currently fixed into the prepared responses payload.
  TypeParam testee =
      this->makeProvider(empty, expectedVerticesEdgesBundleToFetch);
  std::string startString = "v/0";
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);

  if (start.isLooseEnd()) {
    std::vector<decltype(start)*> looseEnds{};
    looseEnds.emplace_back(&start);
    auto futures = testee.fetch(looseEnds);
    auto steps = futures.waitAndGet();
  }

  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  EXPECT_EQ(result.size(), 0U);
  TraversalStats stats = testee.stealStats();
  EXPECT_EQ(stats.getFiltered(), 0U);

  if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                              SingleServerProviderStep>> ||
                std::is_same_v<TypeParam, MockGraphProvider>) {
    EXPECT_EQ(stats.getHttpRequests(), 0U);
  } else if (std::is_same_v<TypeParam, ClusterProvider<ClusterProviderStep>>) {
    EXPECT_EQ(stats.getHttpRequests(), 2U);
  }

  // We have no edges, so nothing scanned in the Index.
  EXPECT_EQ(stats.getScannedIndex(), 0U);
}

TYPED_TEST(GraphProviderTest, should_enumerate_a_single_edge) {
  MockGraph g{};
  g.addEdge(0, 1);

  std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> const&
      expectedVerticesEdgesBundleToFetch = {{0, {}}};
  // IMPORTANT Note: As soon as we're going to include vertices here with a
  // depth > 1, we have to adjust the API of MockGraph::simulateApi. As
  // This value is currently fixed into the prepared responses payload.

  auto testee = this->makeProvider(g, expectedVerticesEdgesBundleToFetch);
  std::string startString = "v/0";
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);

  if (start.isLooseEnd()) {
    std::vector<decltype(start)*> looseEnds{};
    looseEnds.emplace_back(&start);
    auto futures = testee.fetch(looseEnds);
    auto steps = futures.waitAndGet();
  }

  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&result](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  ASSERT_EQ(result.size(), 1U);
  auto const& f = result.at(0);
  EXPECT_EQ(f.getVertex().getID().toString(), "v/1");
  EXPECT_EQ(f.getPrevious(), 0U);

  {
    TraversalStats stats = testee.stealStats();
    EXPECT_EQ(stats.getFiltered(), 0U);
    if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                                SingleServerProviderStep>> ||
                  std::is_same_v<TypeParam, MockGraphProvider>) {
      EXPECT_EQ(stats.getHttpRequests(), 0U);
    } else if (std::is_same_v<TypeParam,
                              ClusterProvider<ClusterProviderStep>>) {
      EXPECT_EQ(stats.getHttpRequests(), 2U);
    }

    if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                                SingleServerProviderStep>> ||
                  std::is_same_v<TypeParam, MockGraphProvider>) {
      // We have 1 edge, this shall be counted
      EXPECT_EQ(stats.getScannedIndex(), 1U);
    } else if (std::is_same_v<TypeParam,
                              ClusterProvider<ClusterProviderStep>>) {
      EXPECT_EQ(stats.getScannedIndex(), 2U);  // we count edge + start vertex
    }
  }
  {
    // Make sure stats are reset after we stole them
    // So steal again works, but on empty statistics
    TraversalStats stats = testee.stealStats();
    EXPECT_EQ(stats.getFiltered(), 0U);
    EXPECT_EQ(stats.getHttpRequests(), 0U);
    EXPECT_EQ(stats.getScannedIndex(), 0U);
  }
}

TYPED_TEST(GraphProviderTest, should_enumerate_all_edges) {
  MockGraph g{};
  g.addEdge(0, 1);
  g.addEdge(0, 2);
  g.addEdge(0, 3);
  std::unordered_set<std::string> found{};

  std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> const&
      expectedVerticesEdgesBundleToFetch = {{0, {}}};
  // IMPORTANT Note: As soon as we're going to include vertices here with a
  // depth > 1, we have to adjust the API of MockGraph::simulateApi. As
  // This value is currently fixed into the prepared responses payload.

  auto testee = this->makeProvider(g, expectedVerticesEdgesBundleToFetch);
  std::string startString = g.vertexToId(0);
  VPackHashedStringRef startH{startString.c_str(),
                              static_cast<uint32_t>(startString.length())};
  auto start = testee.startVertex(startH);

  if (start.isLooseEnd()) {
    std::vector<decltype(start)*> looseEnds{};
    looseEnds.emplace_back(&start);
    auto futures = testee.fetch(looseEnds);
    auto steps = futures.waitAndGet();
  }

  std::vector<typename decltype(testee)::Step> result{};
  testee.expand(start, 0, [&](typename decltype(testee)::Step n) -> void {
    result.emplace_back(std::move(n));
  });

  ASSERT_EQ(result.size(), 3U);
  for (auto const& f : result) {
    // All expand of the same previous
    EXPECT_EQ(f.getPrevious(), 0U);
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
    EXPECT_EQ(stats.getFiltered(), 0U);
    if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                                SingleServerProviderStep>> ||
                  std::is_same_v<TypeParam, MockGraphProvider>) {
      EXPECT_EQ(stats.getHttpRequests(), 0U);
    } else if (std::is_same_v<TypeParam,
                              ClusterProvider<ClusterProviderStep>>) {
      EXPECT_EQ(stats.getHttpRequests(), 2U);
    }
    if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                                SingleServerProviderStep>> ||
                  std::is_same_v<TypeParam, MockGraphProvider>) {
      // We have 3 edges, this shall be counted
      EXPECT_EQ(stats.getScannedIndex(), 3U);
    } else if (std::is_same_v<TypeParam,
                              ClusterProvider<ClusterProviderStep>>) {
      EXPECT_EQ(stats.getScannedIndex(), 4U);  // we count edge + start vertex
    }
  }
}

TYPED_TEST(GraphProviderTest, destroy_engines) {
  MockGraph empty{};
  std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> const&
      expectedVerticesEdgesBundleToFetch = {};
  // IMPORTANT Note: As soon as we're going to include vertices here with a
  // depth > 1, we have to adjust the API of MockGraph::simulateApi. As
  // This value is currently fixed into the prepared responses payload.
  TypeParam testee =
      this->makeProvider(empty, expectedVerticesEdgesBundleToFetch);

  // steel the stats, so we reset them internally and have a clean state
  std::ignore = testee.stealStats();

  testee.destroyEngines();
  TraversalStats statsAfterSteal = testee.stealStats();
  if constexpr (std::is_same_v<TypeParam, SingleServerProvider<
                                              SingleServerProviderStep>> ||
                std::is_same_v<TypeParam, MockGraphProvider>) {
    EXPECT_EQ(statsAfterSteal.getHttpRequests(), 0U);
  } else if (std::is_same_v<TypeParam, ClusterProvider<ClusterProviderStep>>) {
    EXPECT_EQ(statsAfterSteal.getHttpRequests(),
              this->clusterEngines.get()->size());
  }
}

}  // namespace generic_graph_provider_test
}  // namespace tests
}  // namespace arangodb
