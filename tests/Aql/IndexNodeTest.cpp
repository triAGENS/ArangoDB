////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Yuriy Popov
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include "Aql/Ast.h"
#include "Aql/AqlItemBlockSerializationFormat.h"
#include "Aql/IndexNode.h"
#include "Aql/Query.h"
#include "Mocks/Servers.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Transaction/StandaloneContext.h"
#include "velocypack/Iterator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"

namespace {

class IIndexNodeTest
  : public ::testing::Test,
    public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION, arangodb::LogLevel::ERR> {

 protected:
  arangodb::tests::mocks::MockAqlServer server;

  IIndexNodeTest() : server(false) {
    server.startFeatures();
  }

};  // IIndexNodeTest

arangodb::CreateDatabaseInfo createInfo(arangodb::application_features::ApplicationServer& server) {
  arangodb::CreateDatabaseInfo info(server);
  info.allowSystemDB(false);
  auto rv = info.load("testVocbase", 2);
  if (rv.fail()) {
    throw std::runtime_error(rv.errorMessage());
  }
  return info;
}

arangodb::aql::QueryResult executeQuery(TRI_vocbase_t& vocbase, std::string const& queryString,
                                        std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr,
                                        std::string const& optionsString = "{}"
) {
  arangodb::aql::Query query(false, vocbase, arangodb::aql::QueryString(queryString), bindVars,
                             arangodb::velocypack::Parser::fromJson(optionsString),
                             arangodb::aql::PART_MAIN);

  std::shared_ptr<arangodb::aql::SharedQueryState> ss = query.sharedState();

  arangodb::aql::QueryResult result;
  while (true) {
    auto state = query.execute(arangodb::QueryRegistryFeature::registry(), result);
    if (state == arangodb::aql::ExecutionState::WAITING) {
      ss->waitForAsyncResponse();
    } else {
      break;
    }
  }
  return result;
}

TEST_F(IIndexNodeTest, objectQuery) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
    "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_TRUE(collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson("{\"type\": \"hash\", \"fields\": [\"obj.a\", \"obj.b\", \"obj.c\"]}");
  auto createdIndex = false;
  auto index = collection->createIndex(indexJson->slice(), createdIndex);
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(arangodb::transaction::StandaloneContext::Create(vocbase),
                                     EMPTY, EMPTY, EMPTY,
                                     arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  arangodb::ManagedDocumentResult mmdoc;
  auto jsonDocument = arangodb::velocypack::Parser::fromJson("{\"obj\": {\"a\": \"a_val\", \"b\": \"b_val\", \"c\": \"c_val\"}}");
  auto const res = collection->insert(&trx, jsonDocument->slice(), mmdoc, opt, false);
  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(trx.commit().ok());

  {
    auto queryString = "FOR d IN testCollection FILTER d.obj.a == 'a_val' SORT d.obj.c LIMIT 10 RETURN d";
    auto queryResult = ::executeQuery(vocbase, queryString);
    EXPECT_TRUE(queryResult.result.ok()); // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
  }

  // const object in condition
  {
    auto queryString = "FOR d IN testCollection FILTER d.obj.a == {sub_a: \"a_val\"}.sub_a SORT d.obj.c LIMIT 10 RETURN d";
    auto queryResult = ::executeQuery(vocbase, queryString);
    EXPECT_TRUE(queryResult.result.ok()); // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
  }
}

TEST_F(IIndexNodeTest, expansionQuery) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
    "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_TRUE(collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson("{\"type\": \"hash\", \"fields\": [\"tags.hop[*].foo.fo\", \"tags.hop[*].bar.br\", \"tags.hop[*].baz.bz\"]}");
  bool createdIndex{};
  auto index = collection->createIndex(indexJson->slice(), createdIndex);
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(arangodb::transaction::StandaloneContext::Create(vocbase),
                                     EMPTY, EMPTY, EMPTY,
                                     arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  arangodb::ManagedDocumentResult mmdoc;
  auto jsonDocument1 = arangodb::velocypack::Parser::fromJson("{\"tags\": {\"hop\": [{\"foo\": {\"fo\": \"foo_val\"}, \"bar\": {\"br\": \"bar_val\"}, \"baz\": {\"bz\": \"baz_val\"}}]}}");
  auto jsonDocument2 = arangodb::velocypack::Parser::fromJson("{\"tags\": {\"hop\": [{\"foo\": {\"fo\": \"foo_val\"}}, {\"bar\": {\"br\": \"bar_val\"}}, {\"baz\": {\"bz\": \"baz_val\"}}]}}");
  auto const res1 = collection->insert(&trx, jsonDocument1->slice(), mmdoc, opt, false);
  EXPECT_TRUE(res1.ok());
  auto const res2 = collection->insert(&trx, jsonDocument2->slice(), mmdoc, opt, false);
  EXPECT_TRUE(res2.ok());

  EXPECT_TRUE(trx.commit().ok());
  auto queryString = "FOR d IN testCollection FILTER 'foo_val' IN d.tags.hop[*].foo.fo SORT d.tags.hop[*].baz.bz LIMIT 10 RETURN d";
  auto queryResult = ::executeQuery(vocbase, queryString);
  EXPECT_TRUE(queryResult.result.ok()); // commit
  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());
  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(2, resultIt.size());
}

TEST_F(IIndexNodeTest, lastExpansionQuery) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
    "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_TRUE(collection);
  auto indexJson = arangodb::velocypack::Parser::fromJson("{\"type\": \"hash\", \"fields\": [\"tags[*]\"]}");
  bool createdIndex{};
  auto index = collection->createIndex(indexJson->slice(), createdIndex);
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);

  std::vector<std::string> const EMPTY;
  arangodb::transaction::Methods trx(arangodb::transaction::StandaloneContext::Create(vocbase),
                                     EMPTY, EMPTY, EMPTY,
                                     arangodb::transaction::Options());
  EXPECT_TRUE(trx.begin().ok());

  arangodb::OperationOptions opt;
  arangodb::ManagedDocumentResult mmdoc;
  auto jsonDocument = arangodb::velocypack::Parser::fromJson("{\"tags\": [\"foo_val\", \"bar_val\", \"baz_val\"]}");
  auto const res = collection->insert(&trx, jsonDocument->slice(), mmdoc, opt, false);
  EXPECT_TRUE(res.ok());

  EXPECT_TRUE(trx.commit().ok());
  {
    auto queryString = "FOR d IN testCollection FILTER 'foo_val' IN d.tags[*] SORT d.tags LIMIT 10 RETURN d";
    auto queryResult = ::executeQuery(vocbase, queryString);
    EXPECT_TRUE(queryResult.result.ok()); // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
  }
  {
    auto queryString = "FOR d IN testCollection FILTER 'foo_val' IN d.tags SORT d.tags LIMIT 10 RETURN d";
    auto queryResult = ::executeQuery(vocbase, queryString);
    EXPECT_TRUE(queryResult.result.ok()); // commit
    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());
    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(1, resultIt.size());
  }
}

TEST_F(IIndexNodeTest, constructIndexNode) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, createInfo(server.server()));
  // create a collection
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
    "{\"name\": \"testCollection\", \"id\": 42}");
  auto collection = vocbase.createCollection(collectionJson->slice());
  ASSERT_TRUE(collection);
  // create an index node
  auto indexJson = arangodb::velocypack::Parser::fromJson("{\"type\": \"hash\", \"id\": 2086177, \"fields\": [\"obj.a\", \"obj.b\", \"obj.c\"]}");
  bool createdIndex{};
  auto index = collection->createIndex(indexJson->slice(), createdIndex);
  ASSERT_TRUE(createdIndex);
  ASSERT_FALSE(!index);
  // auto jsonDocument = arangodb::velocypack::Parser::fromJson("{\"obj\": {\"a\": \"a_val\", \"b\": \"b_val\", \"c\": \"c_val\"}}");

  // correct json
  auto createJson = arangodb::velocypack::Parser::fromJson(
        "{"
        "  \"IndexesValuesVars\" : ["
        "    {"
        "      \"IndexValuesVars\" : ["
        "        {"
        "          \"fieldNumber\" : 2,"
        "          \"id\" : 6,"
        "          \"name\" : \"5\""
        "        }"
        "      ],"
        "      \"indexId\" : 2086177"
        "    }"
        "  ],"
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
        "  \"outNmColName\" : \"testCollection\","
        "  \"outNmDocId\" : {"
        "    \"id\" : 8,"
        "    \"name\" : \"7\""
        "  },"
        "  \"outVariable\" : {"
        "    \"id\" : 0,"
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
        "      \"VariableId\" : 0,"
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
        "  \"varsUsedLater\" : ["
        "    {"
        "      \"id\" : 0,"
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
        "  ],"
        "  \"varsValid\" : ["
        "    {"
        "      \"id\" : 8,"
        "      \"name\" : \"7\""
        "    },"
        "    {"
        "      \"id\" : 6,"
        "      \"name\" : \"5\""
        "    }"
        "  ]"
        "}"
  );

  arangodb::aql::Query query(false, vocbase, arangodb::aql::QueryString(
                               "FOR d IN testCollection FILTER d.obj.a == 'a_val' SORT d.obj.c LIMIT 10 RETURN d"),
                             nullptr, arangodb::velocypack::Parser::fromJson("{}"),
                             arangodb::aql::PART_MAIN);
  query.prepare(arangodb::QueryRegistryFeature::registry(),
                arangodb::aql::SerializationFormat::SHADOWROWS);

  {
    // short path for a test
    {
      auto vars = query.plan()->getAst()->variables();
      for (auto const& v : {std::make_unique<arangodb::aql::Variable>("d", 0),
        std::make_unique<arangodb::aql::Variable>("3", 4),
        std::make_unique<arangodb::aql::Variable>("5", 6),
        std::make_unique<arangodb::aql::Variable>("7", 8)}) {
        if (vars->getVariable(v->id) == nullptr) {
          std::unique_ptr<arangodb::aql::Variable>(vars->createVariable(&*v));
        }
      }
    }

    // deserialization
    arangodb::aql::IndexNode indNode(query.plan(), createJson->slice());
    ASSERT_TRUE(indNode.isLateMaterialized());

    // serialization and deserialization
    {
      VPackBuilder builder;
      std::unordered_set<arangodb::aql::ExecutionNode const*> seen;
      {
        VPackArrayBuilder guard(&builder);
        indNode.toVelocyPackHelper(builder, arangodb::aql::ExecutionNode::SERIALIZE_DETAILS, seen);
      }

      arangodb::aql::IndexNode indNodeDeserialized(query.plan(), createJson->slice());
      ASSERT_TRUE(indNodeDeserialized.isLateMaterialized());
    }

    // clone
    {
      // without properties
      {
        auto indNodeClone = dynamic_cast<arangodb::aql::IndexNode*>(indNode.clone(query.plan(), true, false));

        EXPECT_EQ(indNode.getType(), indNodeClone->getType());
        EXPECT_EQ(indNode.outVariable(), indNodeClone->outVariable());
        EXPECT_EQ(indNode.plan(), indNodeClone->plan());
        EXPECT_EQ(indNode.vocbase(), indNodeClone->vocbase());
        EXPECT_EQ(indNode.isLateMaterialized(), indNodeClone->isLateMaterialized());

        ASSERT_TRUE(indNodeClone->isLateMaterialized());
      }

      // with properties
      {
        arangodb::aql::Query queryClone(false, vocbase, arangodb::aql::QueryString(
                                     "RETURN 1"),
                                   nullptr, arangodb::velocypack::Parser::fromJson("{}"),
                                   arangodb::aql::PART_MAIN);
        queryClone.prepare(arangodb::QueryRegistryFeature::registry(),
                           arangodb::aql::SerializationFormat::SHADOWROWS);
        indNode.invalidateVarUsage();
        auto indNodeClone = dynamic_cast<arangodb::aql::IndexNode*>(indNode.clone(queryClone.plan(), true, true));

        EXPECT_EQ(indNode.getType(), indNodeClone->getType());
        EXPECT_NE(indNode.outVariable(), indNodeClone->outVariable());
        EXPECT_NE(indNode.plan(), indNodeClone->plan());
        EXPECT_EQ(indNode.vocbase(), indNodeClone->vocbase());
        EXPECT_EQ(indNode.isLateMaterialized(), indNodeClone->isLateMaterialized());

        ASSERT_TRUE(indNodeClone->isLateMaterialized());
      }
    }

    // not materialized
    {
      indNode.setLateMaterialized(nullptr, arangodb::aql::IndexNode::IndexVarsInfo());
      ASSERT_FALSE(indNode.isLateMaterialized());
    }
  }
}

}
