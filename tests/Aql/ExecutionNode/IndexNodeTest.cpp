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
/// @author Yuriy Popov
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionNode/IndexNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Aql/SharedQueryState.h"
#include "Async/async.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Cluster/ServerState.h"
#include "Mocks/Servers.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/OperationOrigin.h"
#include "VocBase/Identifiers/RevisionId.h"
#include "VocBase/LogicalCollection.h"

#include "IResearch/common.h"

#include <velocypack/Iterator.h>

namespace {

class IndexNodeTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;
  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor resourceMonitor{global};

  IndexNodeTest() : server(false) {
    // otherwise asserts fail
    arangodb::ServerState::instance()->setRole(
        arangodb::ServerState::ROLE_SINGLE);
    server.startFeatures();
  }

};  // IndexNodeTest

arangodb::CreateDatabaseInfo createInfo(arangodb::ArangodServer& server) {
  arangodb::CreateDatabaseInfo info(server, arangodb::ExecContext::current());
  auto rv = info.load("testVocbase", 2);
  if (rv.fail()) {
    throw std::runtime_error(rv.errorMessage().data());
  }
  return info;
}

arangodb::aql::QueryResult executeQuery(
    const std::shared_ptr<arangodb::transaction::Context>& ctx,
    std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr,
    std::string const& optionsString = "{}") {
  auto query = arangodb::aql::Query::create(
      ctx, arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(
          arangodb::velocypack::Parser::fromJson(optionsString)->slice()));

  arangodb::aql::QueryResult result;
  while (true) {
    auto state = query->execute(result);
    if (state == arangodb::aql::ExecutionState::WAITING) {
      query->sharedState()->waitForAsyncWakeup();
    } else {
      break;
    }
  }
  return result;
}

TEST_F(IndexNodeTest, objectQuery) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"fields\": [\"obj.a\", \"obj.b\", \"obj.c\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      EMPTY, {collection->name()}, EMPTY, arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  auto jsonDocument = arangodb::velocypack::Parser::fromJson(
      "{\"_key\": \"doc\", \"obj\": {\"a\": \"a_val\", \"b\": \"b_val\", "
      "\"c\": \"c_val\"}}");
  auto res = trx.insert(collection->name(), jsonDocument->slice(), opt);
  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(trx.commit().ok());

  {
    auto queryString =
        "FOR d IN testCollection FILTER d.obj.a == 'a_val' SORT d.obj.c LIMIT "
        "10 RETURN d";
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto queryResult = ::executeQuery(ctx, queryString);
    EXPECT_TRUE(queryResult.result.ok());  // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
    ASSERT_EQ(jsonDocument->slice().get("_key").toJson(),
              resultIt.value().get("_key").toJson());
  }

  // const object in condition
  {
    auto queryString =
        "FOR d IN testCollection FILTER d.obj.a == {sub_a: \"a_val\"}.sub_a "
        "SORT d.obj.c LIMIT 10 RETURN d";
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto queryResult = ::executeQuery(ctx, queryString);
    EXPECT_TRUE(queryResult.result.ok());  // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
    ASSERT_EQ(jsonDocument->slice().get("_key").toJson(),
              resultIt.value().get("_key").toJson());
  }

  // two index variables for registers
  {
    auto queryString =
        "FOR d IN testCollection FILTER d.obj.a == 'a_val' SORT d.obj.c LIMIT "
        "2 SORT d.obj.b DESC LIMIT 1 RETURN d";
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto queryResult = ::executeQuery(ctx, queryString);
    EXPECT_TRUE(queryResult.result.ok());  // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
    ASSERT_EQ(jsonDocument->slice().get("_key").toJson(),
              resultIt.value().get("_key").toJson());
  }
}

TEST_F(IndexNodeTest, expansionQuery) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"fields\": [\"tags.hop[*].foo.fo\", "
      "\"tags.hop[*].bar.br\", \"tags.hop[*].baz.bz\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      EMPTY, {collection->name()}, EMPTY, arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  auto jsonDocument0 = arangodb::velocypack::Parser::fromJson(
      "{\"_key\": \"doc_0\", \"tags\": {\"hop\": [{\"foo\": {\"fo\": "
      "\"foo_val\"}, \"bar\": {\"br\": \"bar_val\"}, \"baz\": {\"bz\": "
      "\"baz_val_0\"}}]}}");
  auto jsonDocument1 = arangodb::velocypack::Parser::fromJson(
      "{\"_key\": \"doc_1\", \"tags\": {\"hop\": [{\"foo\": {\"fo\": "
      "\"foo_val\"}}, {\"bar\": {\"br\": \"bar_val\"}}, {\"baz\": {\"bz\": "
      "\"baz_val_1\"}}]}}");
  auto res0 = trx.insert(collection->name(), jsonDocument0->slice(), opt);
  EXPECT_TRUE(res0.ok());

  auto res1 = trx.insert(collection->name(), jsonDocument1->slice(), opt);
  EXPECT_TRUE(res1.ok());
  EXPECT_TRUE(trx.commit().ok());
  auto queryString =
      "FOR d IN testCollection FILTER 'foo_val' IN d.tags.hop[*].foo.fo SORT "
      "d.tags.hop[*].baz.bz LIMIT 2 RETURN d";
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, arangodb::transaction::OperationOriginTestCase{});
  auto queryResult = ::executeQuery(ctx, queryString);
  EXPECT_TRUE(queryResult.result.ok());  // commit
  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());
  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(2, resultIt.size());
  ASSERT_EQ(jsonDocument1->slice().get("_key").toJson(),
            resultIt.value().get("_key").toJson());
  ASSERT_EQ(jsonDocument0->slice().get("_key").toJson(),
            (++resultIt).value().get("_key").toJson());
}

TEST_F(IndexNodeTest, expansionIndexAndNotExpansionDocumentQuery) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"fields\": [\"tags.hop[*].foo.fo\", "
      "\"tags.hop[*].bar.br\", \"tags.hop[*].baz.bz\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      EMPTY, {collection->name()}, EMPTY, arangodb::transaction::Options());

  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  auto jsonDocument = arangodb::velocypack::Parser::fromJson(
      "{\"tags\": {\"hop\": {\"foo\": {\"fo\": \"foo_val\"}, \"bar\": {\"br\": "
      "\"bar_val\"}, \"baz\": {\"bz\": \"baz_val\"}}}}");
  auto res = trx.insert(collection->name(), jsonDocument->slice(), opt);
  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(trx.commit().ok());
  auto queryString =
      "FOR d IN testCollection FILTER 'foo_val' IN d.tags.hop[*].foo.fo SORT "
      "d.tags.hop[*].baz.bz LIMIT 10 RETURN d";
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, arangodb::transaction::OperationOriginTestCase{});
  auto queryResult = ::executeQuery(ctx, queryString);
  EXPECT_TRUE(queryResult.result.ok());  // commit
  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());
  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(0, resultIt.size());
}

TEST_F(IndexNodeTest, lastExpansionQuery) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"fields\": [\"tags[*]\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      EMPTY, {collection->name()}, EMPTY, arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  auto jsonDocument = arangodb::velocypack::Parser::fromJson(
      "{\"_key\": \"doc\", \"tags\": [\"foo_val\", \"bar_val\", \"baz_val\"]}");
  auto res = trx.insert(collection->name(), jsonDocument->slice(), opt);
  EXPECT_TRUE(res.ok());

  EXPECT_TRUE(trx.commit().ok());
  {
    auto queryString =
        "FOR d IN testCollection FILTER 'foo_val' IN d.tags[*] SORT d.tags "
        "LIMIT 10 RETURN d";
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto queryResult = ::executeQuery(ctx, queryString);
    EXPECT_TRUE(queryResult.result.ok());  // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
    ASSERT_EQ(jsonDocument->slice().get("_key").toJson(),
              resultIt.value().get("_key").toJson());
  }
  {
    auto queryString =
        "FOR d IN testCollection FILTER 'foo_val' IN d.tags SORT d.tags LIMIT "
        "10 RETURN d";
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto queryResult = ::executeQuery(ctx, queryString);
    EXPECT_TRUE(queryResult.result.ok());  // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
    ASSERT_EQ(jsonDocument->slice().get("_key").toJson(),
              resultIt.value().get("_key").toJson());
  }
}

TEST_F(IndexNodeTest, constructIndexNode) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  // create an index node
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"id\": 2086177, \"fields\": [\"obj.a\", "
      "\"obj.b\", \"obj.c\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);
  // auto jsonDocument = arangodb::velocypack::Parser::fromJson("{\"obj\":
  // {\"a\": \"a_val\", \"b\": \"b_val\", \"c\": \"c_val\"}}");

  // correct json
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{"
      "  \"indexValuesVars\" : ["
      "    {"
      "      \"fieldNumber\" : 2,"
      "      \"id\" : 6,"
      "      \"name\" : \"5\""
      "    }"
      "  ],"
      "  \"indexIdOfVars\" : 2086177,"
      "  \"ascending\" : true,"
      "  \"collection\" : \"testCollection\","
      "  \"condition\" : {"
      "    \"subNodes\" : ["
      "      {"
      "        \"subNodes\" : ["
      "          {"
      "            \"excludesNull\" : false,"
      "            \"subNodes\" : ["
      "              {"
      "                \"name\" : \"a\","
      "                \"subNodes\" : ["
      "                  {"
      "                    \"name\" : \"obj\","
      "                    \"subNodes\" : ["
      "                      {"
      "                        \"id\" : 0,"
      "                        \"name\" : \"d\","
      "                        \"type\" : \"reference\","
      "                        \"typeID\" : 45"
      "                      }"
      "                    ],"
      "                    \"type\" : \"attribute access\","
      "                    \"typeID\" : 35"
      "                  }"
      "                ],"
      "                \"type\" : \"attribute access\","
      "                \"typeID\" : 35"
      "              },"
      "              {"
      "                \"type\" : \"value\","
      "                \"typeID\" : 40,"
      "                \"vType\" : \"string\","
      "                \"vTypeID\" : 4,"
      "                \"value\" : \"a_val\""
      "              }"
      "            ],"
      "            \"type\" : \"compare ==\","
      "            \"typeID\" : 25"
      "          }"
      "        ],"
      "        \"type\" : \"n-ary and\","
      "        \"typeID\" : 62"
      "      }"
      "    ],"
      "    \"type\" : \"n-ary or\","
      "    \"typeID\" : 63"
      "  },"
      "  \"database\" : \"testVocbase\","
      "  \"dependencies\" : ["
      "    1"
      "  ],"
      "  \"depth\" : 1,"
      "  \"evalFCalls\" : true,"
      "  \"id\" : 9,"
      "  \"indexCoversProjections\" : false,"
      "  \"indexes\" : ["
      "    {"
      "      \"deduplicate\" : true,"
      "      \"fields\" : ["
      "        \"obj.a\","
      "        \"obj.b\","
      "        \"obj.c\""
      "      ],"
      "      \"id\" : \"2086177\","
      "      \"name\" : \"idx_1648634948960124928\","
      "      \"selectivityEstimate\" : 1,"
      "      \"sparse\" : false,"
      "      \"type\" : \"hash\","
      "      \"unique\" : false"
      "    }"
      "  ],"
      "  \"isSatellite\" : false,"
      "  \"limit\" : 0,"
      "  \"needsGatherNodeSort\" : false,"
      "  \"nrRegs\" : ["
      "    0,"
      "    3,"
      "    4"
      "  ],"
      "  \"nrRegsHere\" : ["
      "    0,"
      "    3,"
      "    1"
      "  ],"
      "  \"outNmDocId\" : {"
      "    \"id\" : 8,"
      "    \"name\" : \"7\""
      "  },"
      "  \"outVariable\" : {"
      "    \"id\" : 3,"
      "    \"name\" : \"d\""
      "  },"
      "  \"producesResult\" : true,"
      "  \"projections\" : ["
      "  ],"
      "  \"regsToClear\" : ["
      "  ],"
      "  \"reverse\" : false,"
      "  \"satellite\" : false,"
      "  \"sorted\" : true,"
      "  \"totalNrRegs\" : 4,"
      "  \"type\" : \"IndexNode\","
      "  \"typeID\" : 23,"
      "  \"varInfoList\" : ["
      "    {"
      "      \"RegisterId\" : 3,"
      "      \"VariableId\" : 3,"
      "      \"depth\" : 2"
      "    },"
      "    {"
      "      \"RegisterId\" : 2,"
      "      \"VariableId\" : 4,"
      "      \"depth\" : 1"
      "    },"
      "    {"
      "      \"RegisterId\" : 0,"
      "      \"VariableId\" : 8,"
      "      \"depth\" : 1"
      "    },"
      "    {"
      "      \"RegisterId\" : 1,"
      "      \"VariableId\" : 6,"
      "      \"depth\" : 1"
      "    }"
      "  ],"
      "  \"varsUsedLaterStack\" : [ ["
      "    {"
      "      \"id\" : 3,"
      "      \"name\" : \"d\""
      "    },"
      "    {"
      "      \"id\" : 8,"
      "      \"name\" : \"7\""
      "    },"
      "    {"
      "      \"id\" : 4,"
      "      \"name\" : \"3\""
      "    },"
      "    {"
      "      \"id\" : 6,"
      "      \"name\" : \"5\""
      "    }"
      "  ] ],"
      "  \"varsValidStack\" : [ ["
      "    {"
      "      \"id\" : 8,"
      "      \"name\" : \"7\""
      "    },"
      "    {"
      "      \"id\" : 6,"
      "      \"name\" : \"5\""
      "    }"
      "  ] ],"
      "  \"regsToKeepStack\" : [[ ]]"
      "}");

  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, arangodb::transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      ctx,
      arangodb::aql::QueryString(
          std::string_view("FOR d IN testCollection FILTER d.obj.a == 'a_val' "
                           "SORT d.obj.c LIMIT 10 RETURN d")),
      nullptr);
  arangodb::tests::waitForAsync(query->prepareQuery());

  {
    // short path for a test
    {
      auto vars = query->ast()->variables();
      for (auto const& v : {std::make_unique<arangodb::aql::Variable>(
                                "d", 3, false, resourceMonitor),
                            std::make_unique<arangodb::aql::Variable>(
                                "3", 4, false, resourceMonitor),
                            std::make_unique<arangodb::aql::Variable>(
                                "5", 6, false, resourceMonitor),
                            std::make_unique<arangodb::aql::Variable>(
                                "7", 8, false, resourceMonitor)}) {
        if (vars->getVariable(v->id) == nullptr) {
          vars->createVariable(v.get());
        }
      }
    }

    // deserialization
    arangodb::aql::IndexNode indNode(
        const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
        createJson->slice());
    ASSERT_TRUE(indNode.isLateMaterialized());

    // serialization and deserialization
    {
      VPackBuilder builder;
      static_cast<arangodb::aql::ExecutionNode&>(indNode).toVelocyPack(
          builder, arangodb::aql::ExecutionNode::SERIALIZE_DETAILS);

      arangodb::aql::IndexNode indNodeDeserialized(
          const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
          builder.slice());
      ASSERT_TRUE(indNodeDeserialized.isLateMaterialized());
    }

    // clone
    {  // without properties
      {
        auto indNodeClone =
            dynamic_cast<arangodb::aql::IndexNode*>(indNode.clone(
                const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
                true));

        EXPECT_EQ(indNode.getType(), indNodeClone->getType());
        EXPECT_EQ(indNode.outVariable(), indNodeClone->outVariable());
        EXPECT_EQ(indNode.plan(), indNodeClone->plan());
        EXPECT_EQ(indNode.vocbase(), indNodeClone->vocbase());
        EXPECT_EQ(indNode.isLateMaterialized(),
                  indNodeClone->isLateMaterialized());

        ASSERT_TRUE(indNodeClone->isLateMaterialized());
      }
    }

    // not materialized
    {
      indNode.setLateMaterialized(nullptr, arangodb::IndexId::primary(),
                                  arangodb::aql::IndexNode::IndexVarsInfo());
      ASSERT_FALSE(indNode.isLateMaterialized());
    }
  }  // namespace
}

TEST_F(IndexNodeTest, invalidLateMaterializedJSON) {
  TRI_vocbase_t vocbase(createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_FALSE(!collection);
  // create an index node
  auto indexJson = arangodb::velocypack::Parser::fromJson(
      "{\"type\": \"hash\", \"id\": 2086177, \"fields\": [\"obj.a\", "
      "\"obj.b\", \"obj.c\"]}");
  auto createdIndex = false;
  auto index =
      collection->createIndex(indexJson->slice(), createdIndex).waitAndGet();
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, arangodb::transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      ctx,
      arangodb::aql::QueryString(
          std::string_view("FOR d IN testCollection FILTER d.obj.a == 'a_val' "
                           "SORT d.obj.c LIMIT 10 RETURN d")),
      nullptr);
  arangodb::tests::waitForAsync(query->prepareQuery());

  auto vars = query->plan()->getAst()->variables();
  auto const& v =
      std::make_unique<arangodb::aql::Variable>("5", 6, false, resourceMonitor);
  if (vars->getVariable(v->id) == nullptr) {
    vars->createVariable(v.get());
  }

  // correct json
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : 2,"
        "      \"id\" : 6,"
        "      \"name\" : \"5\""
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    arangodb::aql::IndexNode indNode(
        const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
        createJson->slice());
    ASSERT_TRUE(indNode.isLateMaterialized());
  }

  // incorrect indexValuesVars
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : {"
        "    \"fieldNumber\" : 2,"
        "    \"id\" : 6,"
        "    \"name\" : \"5\""
        "  },"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    try {
      arangodb::aql::IndexNode indNode(
          const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
          createJson->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // incorrect fieldNumber
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : \"two\","
        "      \"id\" : 6,"
        "      \"name\" : \"5\""
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    try {
      arangodb::aql::IndexNode indNode(
          const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
          createJson->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // incorrect id
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : 2,"
        "      \"id\" : \"six\","
        "      \"name\" : \"5\""
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    try {
      arangodb::aql::IndexNode indNode(
          const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
          createJson->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // incorrect name
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : 2,"
        "      \"id\" : 6,"
        "      \"name\" : 5"
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    arangodb::aql::IndexNode indNode(
        const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
        createJson->slice());
    ASSERT_TRUE(indNode.isLateMaterialized());  // do not read the name
  }

  // incorrect indexIdOfVars
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : 2,"
        "      \"id\" : 6,"
        "      \"name\" : \"5\""
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : \"2086177\","
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  }, "
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    try {
      arangodb::aql::IndexNode indNode(
          const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
          createJson->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // no outNmDocId
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"indexValuesVars\" : ["
        "    {"
        "      \"fieldNumber\" : 2,"
        "      \"id\" : 6,"
        "      \"name\" : \"5\""
        "    }"
        "  ],"
        "  \"indexIdOfVars\" : 2086177,"
        "  \"collection\" : \"testCollection\","
        "  \"condition\" : {"
        "  },"
        "  \"depth\" : 1,"
        "  \"id\" : 9,"
        "  \"indexes\" : ["
        "    {"
        "      \"deduplicate\" : true,"
        "      \"fields\" : ["
        "        \"obj.a\","
        "        \"obj.b\","
        "        \"obj.c\""
        "      ],"
        "      \"id\" : \"2086177\","
        "      \"name\" : \"idx_1648634948960124928\","
        "      \"selectivityEstimate\" : 1,"
        "      \"sparse\" : false,"
        "      \"type\" : \"hash\","
        "      \"unique\" : false"
        "    }"
        "  ],"
        "  \"nrRegs\" : ["
        "  ],"
        "  \"nrRegsHere\" : ["
        "  ],"
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
        "    \"name\" : \"d\""
        "  },"
        "  \"regsToClear\" : ["
        "  ],"
        "  \"totalNrRegs\" : 0,"
        "  \"varInfoList\" : ["
        "  ],"
        "  \"varsUsedLaterStack\" : [ ["
        "  ] ],"
        "  \"varsValidStack\" : [ ["
        "  ] ]"
        "}");
    // deserialization
    arangodb::aql::IndexNode indNode(
        const_cast<arangodb::aql::ExecutionPlan*>(query->plan()),
        createJson->slice());
    ASSERT_FALSE(indNode.isLateMaterialized());
  }
}
}  // namespace
