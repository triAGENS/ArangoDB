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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "gtest/gtest.h"

#include "IResearch/common.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"
#include "Mocks/StorageEngineMock.h"
#include "Mocks/MockQuery.h"

#include "MockGraph.h"

#include "Aql/AqlFunctionFeature.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Graph/ShortestPathOptions.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utils/VersionTracker.h"
#include "VocBase/LogicalCollection.h"

#include <optional>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::graph;
using namespace arangodb::velocypack;
using namespace arangodb::tests::mocks;

namespace arangodb {
namespace tests {
namespace graph {

struct GraphTestSetup
    : public arangodb::tests::LogSuppressor<arangodb::Logger::FIXME,
                                            arangodb::LogLevel::ERR> {
  arangodb::ArangodServer server;
  StorageEngineMock engine;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<
      std::pair<arangodb::application_features::ApplicationFeature&, bool>>
      features;

  GraphTestSetup();
  ~GraphTestSetup();
};  // Setup

struct MockIndexHelpers {
  static std::shared_ptr<Index> getEdgeIndexHandle(
      TRI_vocbase_t& vocbase, std::string const& edgeCollectionName);

  static arangodb::aql::AstNode* buildCondition(aql::Query& query,
                                                aql::Variable const* tmpVar,
                                                TRI_edge_direction_e direction);
};

struct MockGraphDatabase {
  TRI_vocbase_t vocbase;
  VersionTracker versionTracker;

  MockGraphDatabase(ArangodServer& server, std::string name)
      : vocbase(createInfo(server, name, 1), versionTracker, true) {}

  ~MockGraphDatabase() {}

  // Create a collection with name <name> and <n> vertices
  // with ids 0..n-1
  void addVertexCollection(std::string name, size_t n) {
    auto vertices = generateVertexCollection(name);

    std::vector<std::shared_ptr<arangodb::velocypack::Builder>> docs;
    for (size_t i = 0; i < n; i++) {
      docs.emplace_back(arangodb::velocypack::Parser::fromJson(
          "{ \"_key\": \"" + std::to_string(i) + "\"}"));
    };

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        *vertices, arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE((trx.begin().ok()));

    std::vector<velocypack::Builder> insertedDocs;
    for (auto& entry : docs) {
      auto res = trx.insert(vertices->name(), entry->slice(), options);
      EXPECT_TRUE((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    EXPECT_TRUE((trx.commit().ok()));
    EXPECT_TRUE(insertedDocs.size() == n);
    EXPECT_TRUE(vertices->type());
  }

  struct EdgeDef {
    EdgeDef(size_t from, size_t to) : _from(from), _to(to){};
    EdgeDef(size_t from, size_t to, double weight)
        : _from(from), _to(to), _weight(weight){};
    size_t _from;
    size_t _to;
    std::optional<double> _weight;
  };

  // Create a collection with name <name> of edges given by <edges>
  void addEdgeCollection(std::string name, std::string vertexCollection,
                         std::vector<EdgeDef> edgedef) {
    auto edges = generateEdgeCollection(name);

    std::vector<std::shared_ptr<arangodb::velocypack::Builder>> docs;

    for (auto& p : edgedef) {
      //      std::cout << "edge: " << vertexCollection << " " << p.first << "
      //      -> "
      //          << p.second << std::endl;
      // This is moderately horrible
      auto docJson =
          p._weight.has_value()
              ? velocypack::Parser::fromJson(
                    "{ \"_from\": \"" + vertexCollection + "/" +
                    std::to_string(p._from) + "\"" + ", \"_to\": \"" +
                    vertexCollection + "/" + std::to_string(p._to) +
                    "\", \"cost\": " + std::to_string(p._weight.value()) + "}")
              : velocypack::Parser::fromJson(
                    "{ \"_from\": \"" + vertexCollection + "/" +
                    std::to_string(p._from) + "\"" + ", \"_to\": \"" +
                    vertexCollection + "/" + std::to_string(p._to) + "\" }");
      docs.emplace_back(docJson);
    }

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        *edges, arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE((trx.begin().ok()));

    std::vector<velocypack::Builder> insertedDocs;
    for (auto& entry : docs) {
      auto res = trx.insert(edges->name(), entry->slice(), options);
      EXPECT_TRUE((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    EXPECT_TRUE((trx.commit().ok()));
    EXPECT_TRUE(insertedDocs.size() == edgedef.size());
  }

  void addGraph(MockGraph const& graph) {
    auto vertices = generateVertexCollection("v");
    auto edges = generateEdgeCollection("e");
    graph.storeData(vocbase, "v", "e");
  }

  auto generateEdgeCollection(std::string name)
      -> std::shared_ptr<arangodb::LogicalCollection> {
    auto createJson = velocypack::Parser::fromJson("{ \"name\": \"" + name +
                                                   "\", \"type\": 3 }");
    std::shared_ptr<arangodb::LogicalCollection> edges =
        vocbase.createCollection(createJson->slice());
    TRI_ASSERT(nullptr != edges);

    auto indexJson = velocypack::Parser::fromJson("{ \"type\": \"edge\" }");
    bool created = false;
    auto index = edges->createIndex(indexJson->slice(), created).waitAndGet();
    TRI_ASSERT(index);
    TRI_ASSERT(created);
    return edges;
  }

  auto generateVertexCollection(std::string name)
      -> std::shared_ptr<arangodb::LogicalCollection> {
    auto createJson = velocypack::Parser::fromJson("{ \"name\": \"" + name +
                                                   "\", \"type\": 2 }");
    std::shared_ptr<arangodb::LogicalCollection> vertices =
        vocbase.createCollection(createJson->slice());
    TRI_ASSERT(nullptr != vertices);
    return vertices;
  }

  std::shared_ptr<Index> getEdgeIndexHandle(std::string name) {
    return MockIndexHelpers::getEdgeIndexHandle(vocbase, name);
  }

  std::shared_ptr<arangodb::aql::Query> getQuery(
      std::string qry, std::vector<std::string> collections) {
    auto queryString = arangodb::aql::QueryString(qry);

    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, transaction::OperationOriginTestCase{});
    auto query = std::make_shared<MockQuery>(std::move(ctx), queryString);
    for (auto const& c : collections) {
      query->collections().add(c, AccessMode::Type::READ,
                               arangodb::aql::Collection::Hint::Collection);
    }
    auto aborter = std::make_shared<arangodb::aql::QueryAborter>(query);
    query->prepareQuery(aborter);

    return query;
  }

  std::unique_ptr<arangodb::graph::ShortestPathOptions> getShortestPathOptions(
      arangodb::aql::Query* query) {
    auto plan = const_cast<arangodb::aql::ExecutionPlan*>(query->plan());
    auto ast = plan->getAst();

    auto _toCondition =
        ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);

    auto tmpVar = generateTempVar(query);

    auto _fromCondition = buildOutboundCondition(query, tmpVar);

    AstNode* tmpId1 = plan->getAst()->createNodeReference(tmpVar);
    AstNode* tmpId2 = plan->getAst()->createNodeValueString("", 0);

    {
      auto const* access =
          ast->createNodeAttributeAccess(tmpId1, StaticStrings::ToString);
      auto const* cond = ast->createNodeBinaryOperator(
          NODE_TYPE_OPERATOR_BINARY_EQ, access, tmpId2);
      _toCondition->addMember(cond);
    }

    auto spo = std::make_unique<ShortestPathOptions>(*query);
    spo->setVariable(tmpVar);
    spo->addLookupInfo(plan, "e", StaticStrings::FromString,
                       _fromCondition->clone(ast), /*onlyEdgeIndexes*/ false,
                       TRI_EDGE_OUT);
    spo->addReverseLookupInfo(plan, "e", StaticStrings::ToString,
                              _toCondition->clone(ast),
                              /*onlyEdgeIndexes*/ false, TRI_EDGE_IN);
    return spo;
  }

  arangodb::aql::AstNode* buildOutboundCondition(
      arangodb::aql::Query* query, arangodb::aql::Variable const* tmpVar) {
    return MockIndexHelpers::buildCondition(*query, tmpVar, TRI_EDGE_OUT);
  }

  arangodb::aql::AstNode* buildInboundCondition(
      arangodb::aql::Query* query, arangodb::aql::Variable const* tmpVar) {
    return MockIndexHelpers::buildCondition(*query, tmpVar, TRI_EDGE_IN);
  }

  arangodb::aql::Variable* generateTempVar(arangodb::aql::Query* query) {
    auto plan = const_cast<arangodb::aql::ExecutionPlan*>(query->plan());
    return plan->getAst()->variables()->createTemporaryVariable();
  }
};

}  // namespace graph
}  // namespace tests
}  // namespace arangodb
