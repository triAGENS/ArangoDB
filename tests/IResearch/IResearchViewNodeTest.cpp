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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "absl/cleanup/cleanup.h"

#include "velocypack/Iterator.h"

#include "IResearch/common.h"
#include "Mocks/IResearchLinkMock.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"
#include "Mocks/StorageEngineMock.h"
#include "IResearch/MakeViewSnapshot.h"

#include "Aql/AqlFunctionFeature.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/IResearchViewNode.h"
#include "Aql/ExecutionNode/SingletonNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Executor/IResearchViewExecutor.h"
#include "Aql/Executor/NoResultsExecutor.h"
#include "Aql/Query.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryProfile.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Async/async.h"
#include "Basics/DownCast.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchView.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Sharding/ShardingFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/DatabaseGuard.h"
#include "Utils/SingleCollectionTransaction.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"

namespace {

class IResearchViewNodeTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;
  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor resourceMonitor{global};

  IResearchViewNodeTest() : server(false) {
    arangodb::tests::init(true);

    server.addFeature<arangodb::FlushFeature>(false);
    server.startFeatures();

    auto& dbPathFeature = server.getFeature<arangodb::DatabasePathFeature>();
    arangodb::tests::setDatabasePath(
        dbPathFeature);  // ensure test data is stored in a unique directory
  }
};  // IResearchViewNodeSetup

struct MockQuery final : arangodb::aql::Query {
  MockQuery(std::shared_ptr<arangodb::transaction::Context> const& ctx,
            arangodb::aql::QueryString const& queryString)
      : arangodb::aql::Query{ctx, queryString, nullptr, {}, nullptr} {}

  ~MockQuery() final {
    // Destroy this query, otherwise it's still
    // accessible while the query is being destructed,
    // which can result in a data race on the vptr
    destroy();
  }

  arangodb::transaction::Methods& trxForOptimization() final {
    // original version contains an assertion
    return *_trx;
  }
};

}  // namespace

TEST_F(IResearchViewNodeTest, constructSortedView) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ "
      "  \"name\": \"testView\", "
      "  \"type\": \"arangosearch\", "
      "  \"primarySort\": [ "
      "    { \"field\": \"my.nested.Fields\", \"asc\": false }, "
      "    { \"field\": \"another.field\", \"asc\": true } ] "
      "}");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_TRUE(logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());
  arangodb::aql::Variable const outVariable("variable", 1, false,
                                            resourceMonitor);

  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":1 }, \"options\": { \"waitForSync\" : "
        "true, \"collections\":[42] }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", "
        "\"primarySortBuckets\": 2 }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_TRUE(node.sort().first);  // primary sort is set
    EXPECT_EQ(2, node.sort().second);

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_TRUE(node.options().restrictSources);
    EXPECT_EQ(1, node.options().sources.size());
    EXPECT_EQ(42, node.options().sources.begin()->id());
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }

  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":1 }, \"options\": { \"waitForSync\" : "
        "true, \"collections\":[42] }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", "
        "\"primarySortBuckets\": 1 }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_TRUE(node.sort().first);  // primary sort is set
    EXPECT_EQ(1, node.sort().second);

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_TRUE(node.options().restrictSources);
    EXPECT_EQ(1, node.options().sources.size());
    EXPECT_EQ(42, node.options().sources.begin()->id());
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }

  // invalid 'primarySortBuckets' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":1 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", "
        "\"primarySortBuckets\": false }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'primarySortBuckets' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":1 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", "
        "\"primarySortBuckets\": 3 }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }
}

TEST_F(IResearchViewNodeTest, construct) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());
  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no options
  {
    arangodb::aql::SingletonNode singleton(query.plan(),
                                           arangodb::aql::ExecutionNodeId{0});
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,  // out variable
        nullptr,      // no filter condition
        nullptr,      // no options
        {});          // no sort condition
    node.addDependency(&singleton);

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(1., node.getCost().estimatedCost);
    EXPECT_EQ(0, node.getCost().estimatedNrItems);
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }

  // with options
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    attributeName.setStringValue("waitForSync", strlen("waitForSync"));
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kAuto,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }

  // with options default optimization
  {
    // build options node
    std::string value{"auto"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kAuto,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // with options none optimization
  {
    // build options node
    std::string value{"none"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNone,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // with options noneg optimization
  {
    // build options node
    std::string value{"noneg"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNoNegation,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // with options nodnf optimization
  {
    // build options node
    std::string value{"nodnf"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNoDNF,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // with options exact countApproximate
  {
    // build options node
    std::string value{"exact"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kAuto,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // with optionscost countApproximate
  {
    // build options node
    std::string value{"cost"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kAuto,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Cost);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());
  }
  // invalid options
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    attributeName.setStringValue("waitForSync", strlen("waitForSync"));
    attributeName.addMember(&attributeName);
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    EXPECT_ANY_THROW(arangodb::iresearch::IResearchViewNode(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,         // database
        logicalView,     // view
        outVariable,     // out variable
        nullptr,         // no filter condition
        &options, {}));  // no sort condition
  }
  // invalid option conditionOptimization
  {
    // build options node
    std::string value{"none2"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    EXPECT_ANY_THROW(arangodb::iresearch::IResearchViewNode(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,         // database
        logicalView,     // view
        outVariable,     // out variable
        nullptr,         // no filter condition
        &options, {}));  // no sort condition
  }

  // invalid option conditionOptimization non-string
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(false);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"conditionOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    EXPECT_ANY_THROW(arangodb::iresearch::IResearchViewNode(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,         // database
        logicalView,     // view
        outVariable,     // out variable
        nullptr,         // no filter condition
        &options, {}));  // no sort condition
  }
  // invalid option countApproximate non-string
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(false);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    EXPECT_ANY_THROW(arangodb::iresearch::IResearchViewNode(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,         // database
        logicalView,     // view
        outVariable,     // out variable
        nullptr,         // no filter condition
        &options, {}));  // no sort condition
  }
  // invalid option countApproximate invalid string
  {
    // build options node
    std::string value{"unknown_count_approximate"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    EXPECT_ANY_THROW(arangodb::iresearch::IResearchViewNode(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,         // database
        logicalView,     // view
        outVariable,     // out variable
        nullptr,         // no filter condition
        &options, {}));  // no sort condition
  }
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.setStringValue("waitForSync1", strlen("waitForSync1"));
    attributeName.addMember(&attributeValue);
    attributeName.addMember(&attributeName);
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(),  // plan
        arangodb::aql::ExecutionNodeId{42},
        vocbase,        // database
        logicalView,    // view
        outVariable,    // out variable
        nullptr,        // no filter condition
        &options, {});  // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(&outVariable, &node.outVariable());
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(&outVariable, setHere[0]);
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(0, node.getHeapSortLimit());
    EXPECT_TRUE(node.getHeapSort().empty());

    std::vector<arangodb::iresearch::HeapSortElement> scorersSort{
        {"", 1, std::numeric_limits<size_t>::max(), true},
        {"foo", 2, 1, false},
        {"", -1, 0, true}};
    auto expectedScorersSort = scorersSort;
    node.setHeapSort(std::move(scorersSort), 42);
    EXPECT_EQ(42, node.getHeapSortLimit());
    auto actualScorersSort = node.getHeapSort();
    EXPECT_TRUE(std::equal(expectedScorersSort.begin(),
                           expectedScorersSort.end(), actualScorersSort.begin(),
                           actualScorersSort.end()));
  }
}

TEST_F(IResearchViewNodeTest, constructFromVPackSingleServer) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(
          std::string_view("LET variable = 42 LET scoreVariable1 = "
                           "1 LET scoreVariable2 = 2 RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());
  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // missing 'viewId'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 } }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'viewId' type (string expected)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\":123 }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'viewId' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"foo\"}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, ex.code());
    }
  }

  // invalid 'primarySortBuckets' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"primarySortBuckets\": false }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // only 'scorersSortLimit' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\": 100 }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // only 'scorersSort' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSort\": [{\"index\":1, \"asc\":true}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'scorersSort.index' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":10,  \"scorersSort\": [{\"index\":true, "
        "\"asc\":true}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'scorersSort.asc' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":10,  \"scorersSort\": [{\"index\":1, "
        "\"asc\":42}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'scorersSort' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":10,  \"scorersSort\":false }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'scorersSortLimit' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":false,  \"scorersSort\": [{\"index\":1, "
        "\"asc\":true}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'heapSortLimit' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"heapSortLimit\":false, \"heapSort\": [{\"postfix\":\"\", "
        "\"source\":0, \"field\":0, "
        "\"asc\":true}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // invalid 'heapSort' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"heapSortLimit\":42, \"heapSort\": [{\"postfix\":\"\", "
        "\"source\":0, \"field\":true, "
        "\"asc\":true}]}");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // valid 'heapSort' specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"heapSortLimit\":42, \"heapSort\": [{\"postfix\":\"foo\", "
        "\"source\":0, \"field\":2, "
        "\"asc\":false}]}");
    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());
    auto sorts = node.getHeapSort();
    EXPECT_EQ(sorts.size(), 1);
    EXPECT_FALSE(sorts.front().ascending);
    EXPECT_EQ(sorts.front().fieldNumber, 2);
    EXPECT_EQ(sorts.front().source, 0);
    EXPECT_EQ(sorts.front().postfix, "foo");
    ASSERT_EQ(node.getHeapSortLimit(), 42);
  }

  // no options
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_FALSE(node.options().forceSync);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
  }

  // no options
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" } ");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_FALSE(node.options().forceSync);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
  }

  // no options
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_FALSE(node.options().forceSync);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
  }

  // no options with scorersSort
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":42, \"scorersSort\":[{\"index\":0, "
        "\"asc\":true}, {\"index\":1, \"asc\":false}], "
        "\"scorers\": [ { \"id\": 1, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"TFIDF\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}, "
        "{ \"id\": 2, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"BM25\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}] "
        " }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_EQ(2, node.scorers().size());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(3, setHere.size());
    EXPECT_FALSE(node.options().forceSync);
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(42, node.getHeapSortLimit());
    auto actualScorersSort = node.getHeapSort();
    EXPECT_EQ(2, actualScorersSort.size());
    EXPECT_EQ(0, actualScorersSort[0].source);
    EXPECT_TRUE(actualScorersSort[0].ascending);
    EXPECT_TRUE(actualScorersSort[0].isScore());
    EXPECT_EQ(1, actualScorersSort[1].source);
    EXPECT_FALSE(actualScorersSort[1].ascending);
    EXPECT_TRUE(actualScorersSort[1].isScore());
  }

  // no options with invalid index scorersSort
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":42, \"scorersSort\":[{\"index\":10, "
        "\"asc\":true}, {\"index\":1, \"asc\":false}], "
        "\"scorers\": [ { \"id\": 1, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"TFIDF\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}, "
        "{ \"id\": 2, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"BM25\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}] "
        " }");
    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& ex) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, ex.code());
    }
  }

  // with options
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "true, \"collections\":[42] }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_TRUE(node.options().restrictSources);
    EXPECT_EQ(1, node.options().sources.size());
    EXPECT_EQ(42, node.options().sources.begin()->id());
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
  }

  // with options
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        R"({ 
        "id":42, "depth":0, "totalNrRegs":0, "varInfoList":[],
        "nrRegs":[], "nrRegsHere":[], "regsToClear":[],
        "varsUsedLaterStack":[[]], "varsValid":[],
        "outVariable": {
          "name":"variable", "id":0
        },
        "options": { 
          "waitForSync" : true, 
          "collections":[],
          "parallelism": 2
        }, "viewId": ")" +
        std::to_string(logicalView->id().id()) + R"(" })");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_TRUE(node.options().restrictSources);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kAuto,
              node.options().conditionOptimization);  // default value
    EXPECT_EQ(0, node.options().sources.size());
    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(node.options().parallelism, 2);
  }

  // with options none condition optimization
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"conditionOptimization\": \"none\" }, "
        "\"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_FALSE(node.options().restrictSources);
    EXPECT_EQ(0, node.options().sources.size());
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNone,
              node.options().conditionOptimization);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
  }
  // with options noneg conditionOptimization
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"conditionOptimization\": \"noneg\" }, "
        "\"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_FALSE(node.options().restrictSources);
    EXPECT_EQ(0, node.options().sources.size());
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNoNegation,
              node.options().conditionOptimization);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
  }
  // with options nodnf conditionOptimization
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"conditionOptimization\": \"nodnf\" }, "
        "\"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_FALSE(node.options().restrictSources);
    EXPECT_EQ(0, node.options().sources.size());
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNoDNF,
              node.options().conditionOptimization);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
  }

  // with cost countApproximate
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"conditionOptimization\": \"nodnf\","
        "\"countApproximate\":\"cost\" }, "
        "\"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                json->slice());

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_FALSE(node.sort().first);   // primary sort is not set by default
    EXPECT_EQ(0, node.sort().second);  // primary sort is not set by default

    EXPECT_EQ(arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
              node.getType());
    EXPECT_EQ(outVariable.id, node.outVariable().id);
    EXPECT_EQ(outVariable.name, node.outVariable().name);
    EXPECT_EQ(query.plan(), node.plan());
    EXPECT_EQ(arangodb::aql::ExecutionNodeId{42}, node.id());
    EXPECT_EQ(logicalView, node.view());
    EXPECT_TRUE(node.scorers().empty());
    EXPECT_FALSE(node.volatility().first);   // filter volatility
    EXPECT_FALSE(node.volatility().second);  // sort volatility
    arangodb::aql::VarSet usedHere;
    node.getVariablesUsedHere(usedHere);
    EXPECT_TRUE(usedHere.empty());
    auto const setHere = node.getVariablesSetHere();
    EXPECT_EQ(1, setHere.size());
    EXPECT_EQ(outVariable.id, setHere[0]->id);
    EXPECT_EQ(outVariable.name, setHere[0]->name);
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_FALSE(node.options().restrictSources);
    EXPECT_EQ(0, node.options().sources.size());
    EXPECT_EQ(arangodb::aql::ConditionOptimization::kNoDNF,
              node.options().conditionOptimization);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Cost);

    EXPECT_EQ(0., node.getCost().estimatedCost);    // no dependencies
    EXPECT_EQ(0, node.getCost().estimatedNrItems);  // no dependencies
  }

  // invalid option 'waitForSync'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"waitForSync\" : "
        "\"false\"}, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // invalid option 'collections'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"collections\" : "
        "\"false\"}, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // invalid option 'collections'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"collections\" : "
        "{}}, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }

  // invalid option 'primarySortBuckets'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { \"collections\" : "
        "{}}, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", "
        "\"primarySortBuckets\": true }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_TRUE(TRI_ERROR_BAD_PARAMETER == e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // invalid option 'conditionOptimization'
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"options\": { "
        "\"conditionOptimization\" : "
        "\"invalid\"}, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_TRUE(TRI_ERROR_BAD_PARAMETER == e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // outNmColPtr parameter breakes view (old CRDN --> new PRMR)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, \"outNmDocId\": { "
        "\"name\":\"variable101\", \"id\":101 },"
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid late materialization (invalid view values vars)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":{\"fieldNumber\":0, \"id\":101}, "
        "\"noMaterialization\":false, "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid late materialization (invalid field number)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":[{\"fieldNumber\":\"0\", \"id\":101}], "
        "\"noMaterialization\":false, "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid late materialization (invalid variable id)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":[{\"fieldNumber\":0, \"id\":\"101\"}], "
        "\"noMaterialization\":false, "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid no materialization (invalid noMaterialization)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":[], "
        "\"noMaterialization\":1, "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid costApproximate (invalid type)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":[], "
        "\"noMaterialization\":false, "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"countApproximate\":1 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
  // with invalid costApproximate (invalid value)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLater\":[], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"outNmColPtr\": { "
        "\"name\":\"variable100\", \"id\":100 }, "
        "\"outNmDocId\": { \"name\":\"variable100\", \"id\":100 }, "
        "\"viewValuesVars\":[], "
        "\"noMaterialization\":\"unknown_approximate_type\", "
        "\"options\": { \"waitForSync\" : "
        "true, \"collection\":null, \"countApproximate\":1 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) + "\" }");

    try {
      arangodb::iresearch::IResearchViewNode node(*query.plan(),  // plan
                                                  json->slice());
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_BAD_PARAMETER, e.code());
    } catch (...) {
      EXPECT_TRUE(false);
    }
  }
}

// FIXME TODO
// TEST_F(IResearchViewNodeTest, constructFromVPackCluster) {
//}

TEST_F(IResearchViewNodeTest, clone) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());
  arangodb::aql::Variable const outVariable("variable", 3, false,
                                            resourceMonitor);

  // no filter condition, no sort condition, no shards, no options
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,                              // no filter condition
        nullptr,                              // no options
        {});                                  // no sort condition
    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    // clone without properties into the same plan
    {
      auto const nextId = node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(node.plan(), cloned.plan());
      EXPECT_EQ(arangodb::aql::ExecutionNodeId{nextId.id() + 1}, cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }

    // clone without properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(otherQuery.plan(), cloned.plan());
      EXPECT_EQ(node.id(), cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }
  }

  // no filter condition, no sort condition, no shards, options
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    attributeName.setStringValue("waitForSync", strlen("waitForSync"));

    arangodb::aql::AstNode attributeValueParallelism(
        arangodb::aql::NODE_TYPE_VALUE);
    attributeValueParallelism.setValueType(arangodb::aql::VALUE_TYPE_INT);
    attributeValueParallelism.setIntValue(5);
    arangodb::aql::AstNode attributeNameParallelism(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeNameParallelism.addMember(&attributeValueParallelism);
    attributeNameParallelism.setStringValue("parallelism",
                                            strlen("parallelism"));

    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);
    options.addMember(&attributeNameParallelism);
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,                              // no filter condition
        &options, {});                        // no sort condition
    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_EQ(5, node.options().parallelism);
    // clone without properties into the same plan
    {
      auto const nextId = node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(node.plan(), cloned.plan());
      EXPECT_EQ(arangodb::aql::ExecutionNodeId{nextId.id() + 1}, cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.options().parallelism, cloned.options().parallelism);
    }

    // clone without properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(otherQuery.plan(), cloned.plan());
      EXPECT_EQ(node.id(), cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.options().parallelism, cloned.options().parallelism);
    }
  }

  // no filter condition, no sort condition, with shards, no options
  {
    auto& feature =
        server.server().getFeature<arangodb::iresearch::IResearchFeature>();
    feature.setDefaultParallelism(333);
    absl::Cleanup cleanup = [&feature]() noexcept {
      feature.setDefaultParallelism(1);
    };
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_EQ(333, node.options().parallelism);
    node.shards().emplace("s1", arangodb::LogicalView::Indexes{});
    node.shards().emplace("s2", arangodb::LogicalView::Indexes{});

    // clone without properties into the same plan
    {
      auto const nextId = node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(node.plan(), cloned.plan());
      EXPECT_EQ(arangodb::aql::ExecutionNodeId{nextId.id() + 1}, cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.options().parallelism, cloned.options().parallelism);
    }

    // clone without properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(otherQuery.plan(), cloned.plan());
      EXPECT_EQ(node.id(), cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.options().parallelism, cloned.options().parallelism);
    }
  }

  // no filter condition, sort condition, with shards, no options
  {
    arangodb::iresearch::IResearchViewSort sort;
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no scorers
    node.setSort(sort, 0);

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    node.shards().emplace("s1", arangodb::LogicalView::Indexes{});
    node.shards().emplace("s2", arangodb::LogicalView::Indexes{});

    // clone without properties into the same plan
    {
      auto const nextId = node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(node.plan(), cloned.plan());
      EXPECT_EQ(arangodb::aql::ExecutionNodeId{nextId.id() + 1}, cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }

    // clone without properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(otherQuery.plan(), cloned.plan());
      EXPECT_EQ(node.id(), cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }
  }

  // with late materialization
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    arangodb::aql::Variable const outNmDocId("variable101", 101, false,
                                             resourceMonitor);
    node.setLateMaterialized(outNmDocId);
    ASSERT_TRUE(node.isLateMaterialized());
    auto varsSetOriginal = node.getVariablesSetHere();
    ASSERT_EQ(1, varsSetOriginal.size());
    // clone without properties into the same plan
    {
      auto const nextId = node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      auto varsSetCloned = cloned.getVariablesSetHere();
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(node.plan(), cloned.plan());
      EXPECT_EQ(arangodb::aql::ExecutionNodeId{nextId.id() + 1}, cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_EQ(node.getCost(), cloned.getCost());
      EXPECT_EQ(node.isLateMaterialized(), cloned.isLateMaterialized());
      ASSERT_EQ(varsSetOriginal.size(), varsSetCloned.size());
      EXPECT_EQ(varsSetOriginal[0], varsSetCloned[0]);
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }

    // clone without properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      auto varsSetCloned = cloned.getVariablesSetHere();
      EXPECT_TRUE(cloned.collections().empty());
      EXPECT_EQ(node.empty(), cloned.empty());
      EXPECT_EQ(node.shards(), cloned.shards());
      EXPECT_EQ(node.getType(), cloned.getType());
      EXPECT_EQ(&node.outVariable(), &cloned.outVariable());  // same objects
      EXPECT_EQ(otherQuery.plan(), cloned.plan());
      EXPECT_EQ(node.id(), cloned.id());
      EXPECT_EQ(&node.vocbase(), &cloned.vocbase());
      EXPECT_EQ(node.view(), cloned.view());
      EXPECT_EQ(&node.filterCondition(), &cloned.filterCondition());
      EXPECT_EQ(node.scorers(), cloned.scorers());
      EXPECT_EQ(node.volatility(), cloned.volatility());
      EXPECT_EQ(node.options().forceSync, cloned.options().forceSync);
      EXPECT_EQ(node.sort(), cloned.sort());
      EXPECT_TRUE(node.getCost() == cloned.getCost());
      EXPECT_EQ(node.isLateMaterialized(), cloned.isLateMaterialized());
      ASSERT_EQ(varsSetOriginal.size(), varsSetCloned.size());
      EXPECT_EQ(varsSetOriginal[0], varsSetCloned[0]);
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
    }
  }

  // with scorers sort
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    arangodb::aql::Variable const outNmColPtr("variable100", 100, false,
                                              resourceMonitor);
    arangodb::aql::Variable const outNmDocId("variable101", 101, false,
                                             resourceMonitor);
    std::vector<arangodb::iresearch::HeapSortElement> scorersSort{
        {"", 0, 0, true}};
    node.setHeapSort(std::move(scorersSort), 42);
    auto varsSetOriginal = node.getVariablesSetHere();
    // clone without properties into the same plan
    {
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(query.plan(), true));
      auto varsSetCloned = cloned.getVariablesSetHere();
      EXPECT_EQ(varsSetCloned, varsSetOriginal);
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      auto orig = node.getHeapSort();
      auto clone = cloned.getHeapSort();
      EXPECT_TRUE(
          std::equal(orig.begin(), orig.end(), clone.begin(), clone.end()));
    }
    // clone with properties into another plan
    {
      // another dummy query
      MockQuery otherQuery(
          arangodb::transaction::StandaloneContext::create(
              vocbase, arangodb::transaction::OperationOriginTestCase{}),
          arangodb::aql::QueryString(std::string_view("RETURN 1")));
      arangodb::tests::waitForAsync(otherQuery.prepareQuery());

      node.plan()->nextId();
      auto& cloned = dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
          *node.clone(otherQuery.plan(), true));
      auto varsSetCloned = cloned.getVariablesSetHere();
      ASSERT_EQ(varsSetOriginal.size(), varsSetCloned.size());
      EXPECT_EQ(node.getHeapSortLimit(), cloned.getHeapSortLimit());
      EXPECT_EQ(node.getHeapSort().size(), cloned.getHeapSort().size());
      auto orig = node.getHeapSort();
      auto clone = cloned.getHeapSort();
      EXPECT_TRUE(
          std::equal(orig.begin(), orig.end(), clone.begin(), clone.end()));
    }
  }
}

TEST_F(IResearchViewNodeTest, serialize) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(
          std::string_view("let variable = 1 let variable100 = 3 "
                           "let variable101 = 2 RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());

  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no filter condition, no sort condition
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,                              // no filter condition
        nullptr,                              // no options
        {});                                  // no sort condition
    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }
  }

  // no filter condition, no sort condition, options
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    attributeName.setStringValue("waitForSync", strlen("waitForSync"));
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,                              // no filter condition
        &options, {});                        // no sort condition
    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_TRUE(node.options().forceSync);
    EXPECT_EQ(node.options().countApproximate,
              arangodb::iresearch::CountApproximate::Exact);

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }
  }
  // with late materialization
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    arangodb::aql::Variable const outNmDocId("variable101", 2, false,
                                             resourceMonitor);
    node.setLateMaterialized(outNmDocId);

    node.setVarsUsedLater({arangodb::aql::VarSet{&outNmDocId}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.isLateMaterialized(), deserialized.isLateMaterialized());
      auto varsSetHere = deserialized.getVariablesSetHere();
      ASSERT_EQ(1, varsSetHere.size());
      EXPECT_EQ(outNmDocId.id, varsSetHere[0]->id);
      EXPECT_EQ(outNmDocId.name, varsSetHere[0]->name);
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.isLateMaterialized(), deserialized.isLateMaterialized());
      auto varsSetHere = deserialized.getVariablesSetHere();
      ASSERT_EQ(1, varsSetHere.size());
      EXPECT_EQ(outNmDocId.id, varsSetHere[0]->id);
      EXPECT_EQ(outNmDocId.name, varsSetHere[0]->name);
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
      EXPECT_EQ(node.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }
  }
  // with countApproximate cost
  {
    std::string value{"cost"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,   // no filter condition
        &options,  // no options
        {});       // no sort condition

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(deserialized.options().countApproximate,
                arangodb::iresearch::CountApproximate::Cost);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(deserialized.options().countApproximate,
                arangodb::iresearch::CountApproximate::Cost);
    }
  }
  // with countApproximate exact
  {
    std::string value{"exact"};
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_STRING);
    attributeValue.setStringValue(value.c_str(), value.size());
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"countApproximate"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,   // no filter condition
        &options,  // no options
        {});       // no sort condition

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(deserialized.options().countApproximate,
                arangodb::iresearch::CountApproximate::Exact);
    }
  }
  // with allowed merge filters
  {
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_INT);
    attributeValue.setIntValue(
        static_cast<int64_t>(arangodb::iresearch::FilterOptimization::MAX));
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"filterOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,   // no filter condition
        &options,  // no options
        {});       // no sort condition

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(deserialized.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(deserialized.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::MAX);
    }
  }
  // with forbidden merge filters
  {
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_INT);
    attributeValue.setIntValue(
        static_cast<int64_t>(arangodb::iresearch::FilterOptimization::NONE));
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    std::string name{"filterOptimization"};
    attributeName.setStringValue(name.c_str(), name.size());
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,   // no filter condition
        &options,  // no options
        {});       // no sort condition

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(deserialized.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::NONE);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(deserialized.options().filterOptimization,
                arangodb::iresearch::FilterOptimization::NONE);
    }
  }
  // with scorers sort
  {
    MockQuery queryScores(
        arangodb::transaction::StandaloneContext::create(
            vocbase, arangodb::transaction::OperationOriginTestCase{}),
        arangodb::aql::QueryString(
            std::string_view("LET variable = 42 LET scoreVariable1 = 1 LET "
                             "scoreVariable2 = 2 RETURN 1")));
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\":42, \"depth\":0, \"totalNrRegs\":0, \"varInfoList\":[], "
        "\"nrRegs\":[], \"nrRegsHere\":[], \"regsToClear\":[], "
        "\"varsUsedLaterStack\":[[]], \"varsValid\":[], \"outVariable\": { "
        "\"name\":\"variable\", \"id\":0 }, \"viewId\": \"" +
        std::to_string(logicalView->id().id()) +
        "\", \"scorersSortLimit\":42, \"scorersSort\":[{\"index\":0, "
        "\"asc\":true}, {\"index\":1, \"asc\":false}], "
        "\"scorers\": [ { \"id\": 1, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"TFIDF\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}, "
        "{ \"id\": 2, \"name\": \"5\", \"node\": { "
        "\"type\": \"function call\", \"typeID\": 47, \"name\": \"BM25\", "
        "\"subNodes\": [{\"type\": \"array\", \"typeID\": 41, \"sorted\": "
        "false, "
        "\"subNodes\": [{\"type\": \"reference\", \"typeID\": 45, "
        "\"name\": \"d\",\"id\": 0 }]}]}}] "
        " }");

    arangodb::tests::waitForAsync(queryScores.prepareQuery());
    arangodb::aql::Variable const outVariable("variable", 0, false,
                                              resourceMonitor);
    arangodb::aql::Variable const outScore0("variable100", 1, false,
                                            resourceMonitor);
    arangodb::aql::Variable const outScore1("variable101", 2, false,
                                            resourceMonitor);
    arangodb::iresearch::IResearchViewNode node(
        *queryScores.plan(), json->slice());  // no sort condition
    node.setVarsUsedLater(
        {arangodb::aql::VarSet{&outVariable, &outScore0, &outScore1}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();
    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();
    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(
          *queryScores.plan(), nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(deserialized.getHeapSortLimit(), 42);
      EXPECT_EQ(node.getHeapSort().size(), deserialized.getHeapSort().size());
      auto orig = node.getHeapSort();
      auto clone = deserialized.getHeapSort();
      EXPECT_TRUE(
          std::equal(orig.begin(), orig.end(), clone.begin(), clone.end()));
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(deserialized.getHeapSortLimit(), 42);
      EXPECT_EQ(node.getHeapSort().size(), deserialized.getHeapSort().size());
      auto orig = node.getHeapSort();
      auto clone = deserialized.getHeapSort();
      EXPECT_TRUE(
          std::equal(orig.begin(), orig.end(), clone.begin(), clone.end()));
    }
  }
}

TEST_F(IResearchViewNodeTest, serializeSortedView) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"primarySort\" : "
      "[ { \"field\":\"_key\", \"direction\":\"desc\"} ] }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);
  auto& viewImpl =
      arangodb::basics::downCast<arangodb::iresearch::IResearchView>(
          *logicalView);
  EXPECT_FALSE(viewImpl.primarySort().empty());

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(
          std::string_view("let variable = 1 let variable100 = 3 "
                           "let variable101 = 2 RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());

  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no filter condition, no sort condition
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    node.setSort(viewImpl.primarySort(), 1);

    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(1, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(1, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
    }
  }

  // no filter condition, no sort condition, options
  {
    // build options node
    arangodb::aql::AstNode attributeValue(arangodb::aql::NODE_TYPE_VALUE);
    attributeValue.setValueType(arangodb::aql::VALUE_TYPE_BOOL);
    attributeValue.setBoolValue(true);
    arangodb::aql::AstNode attributeName(
        arangodb::aql::NODE_TYPE_OBJECT_ELEMENT);
    attributeName.addMember(&attributeValue);
    attributeName.setStringValue("waitForSync", strlen("waitForSync"));
    arangodb::aql::AstNode options(arangodb::aql::NODE_TYPE_OBJECT);
    options.addMember(&attributeName);

    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,                              // no filter condition
        &options, {});                        // no sort condition
    EXPECT_TRUE(node.empty());                // view has no links
    EXPECT_TRUE(node.collections().empty());  // view has no links
    EXPECT_TRUE(node.shards().empty());
    EXPECT_TRUE(node.options().forceSync);

    node.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(0, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(0, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
    }
  }

  // with late materialization
  {
    arangodb::iresearch::IResearchViewNode node(
        *query.plan(), arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    arangodb::aql::Variable const outNmDocId("variable101", 2, false,
                                             resourceMonitor);
    node.setSort(viewImpl.primarySort(), 1);
    node.setLateMaterialized(outNmDocId);

    node.setVarsUsedLater({arangodb::aql::VarSet{&outNmDocId}});
    node.setVarsValid({{}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();

    arangodb::velocypack::Builder builder;
    unsigned flags = arangodb::aql::ExecutionNode::SERIALIZE_DETAILS;
    node.toVelocyPack(builder, flags);  // object with array of objects

    auto nodeSlice = builder.slice();

    // constructor
    {
      arangodb::iresearch::IResearchViewNode const deserialized(*query.plan(),
                                                                nodeSlice);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(1, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.isLateMaterialized(), deserialized.isLateMaterialized());
      auto varsSetHere = deserialized.getVariablesSetHere();
      ASSERT_EQ(1, varsSetHere.size());
      EXPECT_EQ(outNmDocId.id, varsSetHere[0]->id);
      EXPECT_EQ(outNmDocId.name, varsSetHere[0]->name);
    }

    // factory method
    {
      std::unique_ptr<arangodb::aql::ExecutionNode> deserializedNode(
          arangodb::aql::ExecutionNode::fromVPackFactory(query.plan(),
                                                         nodeSlice));
      auto& deserialized =
          dynamic_cast<arangodb::iresearch::IResearchViewNode&>(
              *deserializedNode);
      EXPECT_EQ(node.empty(), deserialized.empty());
      EXPECT_EQ(node.shards(), deserialized.shards());
      EXPECT_TRUE(deserialized.collections().empty());
      EXPECT_EQ(node.getType(), deserialized.getType());
      EXPECT_EQ(node.outVariable().id, deserialized.outVariable().id);
      EXPECT_EQ(node.outVariable().name, deserialized.outVariable().name);
      EXPECT_EQ(node.plan(), deserialized.plan());
      EXPECT_EQ(node.id(), deserialized.id());
      EXPECT_EQ(&node.vocbase(), &deserialized.vocbase());
      EXPECT_EQ(node.view(), deserialized.view());
      EXPECT_EQ(&node.filterCondition(), &deserialized.filterCondition());
      EXPECT_EQ(node.scorers(), deserialized.scorers());
      EXPECT_EQ(node.volatility(), deserialized.volatility());
      EXPECT_EQ(node.options().forceSync, deserialized.options().forceSync);
      EXPECT_EQ(node.sort(), deserialized.sort());
      EXPECT_EQ(1, node.sort().second);
      EXPECT_EQ(node.getCost(), deserialized.getCost());
      EXPECT_EQ(node.isLateMaterialized(), deserialized.isLateMaterialized());
      auto varsSetHere = deserialized.getVariablesSetHere();
      ASSERT_EQ(1, varsSetHere.size());
      EXPECT_EQ(outNmDocId.id, varsSetHere[0]->id);
      EXPECT_EQ(outNmDocId.name, varsSetHere[0]->name);
    }
  }
}

TEST_F(IResearchViewNodeTest, collections) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));

  std::shared_ptr<arangodb::LogicalCollection> collection0;
  std::shared_ptr<arangodb::LogicalCollection> collection1;

  // create collection0
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection0\", \"id\" : \"42\" }");
    collection0 = vocbase.createCollection(createJson->slice());
    ASSERT_NE(nullptr, collection0);
  }

  // create collection1
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection1\", \"id\" : \"4242\"  }");
    collection1 = vocbase.createCollection(createJson->slice());
    ASSERT_NE(nullptr, collection1);
  }

  // create collection2
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection2\" , \"id\" : \"424242\" }");
    ASSERT_NE(nullptr, vocbase.createCollection(createJson->slice()));
  }

  // create view
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // link collections
  auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "\"testCollection0\": { \"includeAllFields\": true, "
      "\"trackListPositions\": true },"
      "\"testCollection1\": { \"includeAllFields\": true },"
      "\"testCollection2\": { \"includeAllFields\": true }"
      "}}");
  EXPECT_TRUE(logicalView->properties(updateJson->slice(), true, true).ok());

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));

  // register collections with the query
  query.collections().add(std::to_string(collection0->id().id()),
                          arangodb::AccessMode::Type::READ,
                          arangodb::aql::Collection::Hint::None);
  query.collections().add(std::to_string(collection1->id().id()),
                          arangodb::AccessMode::Type::READ,
                          arangodb::aql::Collection::Hint::None);

  // prepare query
  arangodb::tests::waitForAsync(query.prepareQuery());

  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no filter condition, no sort condition
  arangodb::iresearch::IResearchViewNode node(
      *query.plan(), arangodb::aql::ExecutionNodeId{42},
      vocbase,      // database
      logicalView,  // view
      outVariable,
      nullptr,  // no filter condition
      nullptr,  // no options
      {});      // no sort condition
  EXPECT_TRUE(node.shards().empty());
  EXPECT_FALSE(node.empty());  // view has no links
  auto collections = node.collections();
  EXPECT_FALSE(collections.empty());  // view has no links
  EXPECT_EQ(2, collections.size());

  // we expect only collections 'collection0', 'collection1' to be
  // present since 'collection2' is not registered with the query
  std::unordered_set<std::string> expectedCollections{
      std::to_string(collection0->id().id()),
      std::to_string(collection1->id().id())};

  for (auto const& [collection, indexes] : collections) {
    EXPECT_EQ(1, expectedCollections.erase(collection.get().name()));
  }
  EXPECT_TRUE(expectedCollections.empty());
}

TEST_F(IResearchViewNodeTest, createBlockSingleServer) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // create collection0
  std::shared_ptr<arangodb::LogicalCollection> collection0;
  {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection0\", \"id\" : \"42\" }");
    collection0 = vocbase.createCollection(createJson->slice());
    ASSERT_NE(nullptr, collection0);
  }

  // link collections
  auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "\"testCollection0\": { \"includeAllFields\": true, "
      "\"trackListPositions\": true }"
      "}}");
  EXPECT_TRUE(logicalView->properties(updateJson->slice(), true, true).ok());

  // insert into collection
  {
    std::vector<std::string> const EMPTY;

    arangodb::OperationOptions opt;

    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, arangodb::transaction::OperationOriginTestCase{}),
        EMPTY, {collection0->name()}, EMPTY, arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());

    auto json = arangodb::velocypack::Parser::fromJson("{}");
    auto res = trx.insert(collection0->name(), json->slice(), opt);
    EXPECT_TRUE(res.ok());

    EXPECT_TRUE(trx.commit().ok());
    EXPECT_TRUE(
        (arangodb::tests::executeQuery(vocbase,
                                       "FOR d IN testView SEARCH 1 ==1 OPTIONS "
                                       "{ waitForSync: true } RETURN d")
             .result.ok()));  // commit
  }

  // dummy query
  auto ctx = arangodb::transaction::StandaloneContext::create(
      vocbase, arangodb::transaction::OperationOriginTestCase{});
  MockQuery query(ctx,
                  arangodb::aql::QueryString(std::string_view("RETURN 1")));
  query.initForTests();
  //  arangodb::tests::waitForAsync(query.prepareQuery());

  // dummy engine
  arangodb::aql::ExecutionEngine engine(0, query, query.itemBlockManager(),
                                        query.sharedState());
  arangodb::aql::ExecutionPlan plan(query.ast(), false);

  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no filter condition, no sort condition
  {
    arangodb::aql::SingletonNode singleton(&plan,
                                           arangodb::aql::ExecutionNodeId{0});
    arangodb::iresearch::IResearchViewNode node(
        plan, arangodb::aql::ExecutionNodeId{42},
        vocbase,      // database
        logicalView,  // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {});      // no sort condition
    node.addDependency(&singleton);

    // "Trust me, I'm an IT professional"
    singleton.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    singleton.setVarsValid({{}});
    singleton.setRegsToKeep({{}});
    node.setVarsUsedLater({{}});
    node.setVarsValid({arangodb::aql::VarSet{&outVariable}});
    node.setRegsToKeep({{}});
    node.setVarUsageValid();
    singleton.setVarUsageValid();

    node.planRegisters();

    // before transaction has started (no snapshot)
    try {
      auto block = node.createBlock(engine);
      EXPECT_TRUE(false);
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(TRI_ERROR_INTERNAL, e.code());
    }

    arangodb::transaction::Methods trx(ctx);
    ASSERT_TRUE(trx.state());

    // start transaction (put snapshot into)
    auto& view = arangodb::basics::downCast<arangodb::iresearch::IResearchView>(
        *logicalView);
    auto* snapshot =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         view.getLinks(nullptr), &view, view.name());
    EXPECT_TRUE(snapshot == nullptr);
    snapshot = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        view.getLinks(nullptr), &view, view.name());
    ASSERT_TRUE(snapshot != nullptr);
    auto* snapshotFind =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         view.getLinks(nullptr), &view, view.name());
    EXPECT_TRUE(snapshotFind == snapshot);
    auto* snapshotCreate = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        view.getLinks(nullptr), &view, view.name());
    EXPECT_TRUE(snapshotCreate == snapshot);
    auto* snapshotSync = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::SyncAndReplace,
        view.getLinks(nullptr), &view, view.name());
    EXPECT_TRUE(snapshotSync == snapshot);

    // after transaction has started
    {
      auto block = node.createBlock(engine);
      EXPECT_NE(nullptr, block);
      EXPECT_NE(
          nullptr,
          (dynamic_cast<arangodb::aql::ExecutionBlockImpl<
               arangodb::aql::IResearchViewExecutor<
                   arangodb::aql::ExecutionTraits<
                       false, false, false,
                       arangodb::iresearch::MaterializeType::Materialize>>>*>(
              block.get())));
    }
  }
}

// FIXME TODO
// TEST_F(IResearchViewNodeTest, createBlockDBServer) {
//}

TEST_F(IResearchViewNodeTest, createBlockCoordinator) {
  TRI_vocbase_t vocbase(testDBInfo(server.server()));
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_FALSE(!logicalView);

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));
  query.initForTests();
  //  arangodb::tests::waitForAsync(query.prepareQuery());

  // dummy engine
  arangodb::aql::ExecutionEngine engine(0, query, query.itemBlockManager(),
                                        query.sharedState());
  arangodb::aql::ExecutionPlan plan(query.ast(), false);

  // dummy engine
  arangodb::aql::SingletonNode singleton(&plan,
                                         arangodb::aql::ExecutionNodeId{0});
  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);

  // no filter condition, no sort condition
  arangodb::iresearch::IResearchViewNode node(
      plan, arangodb::aql::ExecutionNodeId{42},
      vocbase,      // database
      logicalView,  // view
      outVariable,
      nullptr,  // no filter condition
      nullptr,  // no options
      {});      // no sort condition
  node.addDependency(&singleton);

  singleton.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
  singleton.setVarsValid({{}});
  node.setVarsUsedLater({{}});
  node.setVarsValid({arangodb::aql::VarSet{&outVariable}});
  singleton.setVarUsageValid();
  node.setVarUsageValid();
  singleton.planRegisters();
  node.planRegisters();
  arangodb::ServerState::instance()->setRole(
      arangodb::ServerState::ROLE_COORDINATOR);
  auto emptyBlock = node.createBlock(engine);
  arangodb::ServerState::instance()->setRole(
      arangodb::ServerState::ROLE_SINGLE);
  EXPECT_NE(nullptr, emptyBlock);
  EXPECT_TRUE(
      nullptr !=
      dynamic_cast<
          arangodb::aql::ExecutionBlockImpl<arangodb::aql::NoResultsExecutor>*>(
          emptyBlock.get()));
}

TEST_F(IResearchViewNodeTest, createBlockCoordinatorLateMaterialize) {
  TRI_vocbase_t vocbase(testDBInfo(server.server(), "testVocbase", 1));
  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalView = vocbase.createView(createJson->slice(), false);
  ASSERT_TRUE((false == !logicalView));

  // dummy query
  MockQuery query(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(std::string_view("RETURN 1")));
  arangodb::tests::waitForAsync(query.prepareQuery());

  // dummy engine
  arangodb::aql::ExecutionEngine engine(0, query, query.itemBlockManager(),
                                        query.sharedState());
  arangodb::aql::SingletonNode singleton(query.plan(),
                                         arangodb::aql::ExecutionNodeId{0});
  arangodb::aql::Variable const outVariable("variable", 0, false,
                                            resourceMonitor);
  arangodb::aql::Variable const outNmDocId("variable101", 101, false,
                                           resourceMonitor);

  // no filter condition, no sort condition
  arangodb::iresearch::IResearchViewNode node(
      *query.plan(), arangodb::aql::ExecutionNodeId{42},
      vocbase,      // database
      logicalView,  // view
      outVariable,
      nullptr,  // no filter condition
      nullptr,  // no options
      {});      // no sort condition
  node.addDependency(&singleton);
  node.setLateMaterialized(outNmDocId);
  singleton.setVarsUsedLater({arangodb::aql::VarSet{&outNmDocId}});
  singleton.setVarsValid({{}});
  node.setVarsUsedLater({{}});
  node.setVarsValid({arangodb::aql::VarSet{&outNmDocId}});
  singleton.setVarUsageValid();
  node.setVarUsageValid();
  singleton.planRegisters();
  node.planRegisters();
  arangodb::ServerState::instance()->setRole(
      arangodb::ServerState::ROLE_COORDINATOR);
  auto emptyBlock = node.createBlock(engine);
  arangodb::ServerState::instance()->setRole(
      arangodb::ServerState::ROLE_SINGLE);
  EXPECT_TRUE(nullptr != emptyBlock);
  EXPECT_TRUE(
      nullptr !=
      dynamic_cast<
          arangodb::aql::ExecutionBlockImpl<arangodb::aql::NoResultsExecutor>*>(
          emptyBlock.get()));
}

class IResearchViewVolatitlityTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;
  arangodb::VocbasePtr vocbase;

  IResearchViewVolatitlityTest() : server(false) {
    arangodb::tests::init(true);

    server.addFeature<arangodb::FlushFeature>(false);
    server.startFeatures();

    auto& dbPathFeature = server.getFeature<arangodb::DatabasePathFeature>();
    arangodb::tests::setDatabasePath(
        dbPathFeature);  // ensure test data is stored in a unique directory

    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
    vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
    EXPECT_NE(nullptr, vocbase);
    std::shared_ptr<arangodb::LogicalCollection> collection0;
    {
      auto createJson = arangodb::velocypack::Parser::fromJson(
          "{ \"name\": \"testCollection0\", \"id\" : \"42\" }");
      collection0 = vocbase->createCollection(createJson->slice());
      EXPECT_NE(nullptr, collection0);
    }
    std::shared_ptr<arangodb::LogicalCollection> collection1;
    {
      auto createJson = arangodb::velocypack::Parser::fromJson(
          "{ \"name\": \"testCollection1\", \"id\" : \"43\" }");
      collection1 = vocbase->createCollection(createJson->slice());
      EXPECT_NE(nullptr, collection1);
    }
    arangodb::LogicalView::ptr logicalView0;
    {
      auto createJson = arangodb::velocypack::Parser::fromJson(
          "{ \"name\": \"testView0\", \"type\": \"arangosearch\" }");
      logicalView0 = vocbase->createView(createJson->slice(), false);
      EXPECT_NE(nullptr, logicalView0);
      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": {"
          "\"testCollection0\": { \"includeAllFields\": true, "
          "\"trackListPositions\": true }"
          "}}");
      EXPECT_TRUE(
          logicalView0->properties(updateJson->slice(), true, true).ok());
    }
    arangodb::LogicalView::ptr logicalView1;
    {
      auto createJson = arangodb::velocypack::Parser::fromJson(
          "{ \"name\": \"testView1\", \"type\": \"arangosearch\" }");
      logicalView1 = vocbase->createView(createJson->slice(), false);
      EXPECT_NE(nullptr, logicalView1);
      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": {"
          "\"testCollection1\": { \"includeAllFields\": true, "
          "\"trackListPositions\": true }"
          "}}");
      EXPECT_TRUE(
          logicalView1->properties(updateJson->slice(), true, true).ok());
    }

    std::vector<std::string> EMPTY_VECTOR;
    auto trx = std::make_shared<arangodb::transaction::Methods>(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        EMPTY_VECTOR,
        std::vector<std::string>{collection0->name(), collection1->name()},
        EMPTY_VECTOR, arangodb::transaction::Options());

    EXPECT_TRUE(trx->begin().ok());
    // in collection only one alive doc
    {
      auto aliveDoc0 = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::OperationOptions options;
      EXPECT_TRUE(
          trx->insert(collection0->name(), aliveDoc0->slice(), options).ok());

      auto aliveDoc1 = arangodb::velocypack::Parser::fromJson("{ \"key\": 2 }");
      arangodb::OperationOptions options1;
      EXPECT_TRUE(
          trx->insert(collection0->name(), aliveDoc1->slice(), options).ok());
    }
    {
      auto aliveDoc1 = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::OperationOptions options;
      EXPECT_TRUE(
          trx->insert(collection1->name(), aliveDoc1->slice(), options).ok());
    }
    EXPECT_TRUE(trx->commit().ok());
    // force views sync
    auto res = arangodb::tests::executeQuery(
        *vocbase,
        "FOR s IN testView0 OPTIONS { waitForSync: true } LET kk = s.key "
        "FOR d IN testView1 SEARCH d.key == kk "
        "OPTIONS { waitForSync: true } RETURN d ");
    EXPECT_TRUE(res.ok());
    EXPECT_TRUE(res.data->slice().isArray());
    EXPECT_NE(0, res.data->slice().length());
  }
};

TEST_F(IResearchViewVolatitlityTest, volatilityFilterSubqueryWithVar) {
  std::string const queryString =
      "FOR s IN testView0 LET kk = s.key "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(1, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterSubquery) {
  std::string const queryString =
      "FOR s IN testView0 "
      "FOR d IN testView1 SEARCH d.key == s.key RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(1, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterNonDetVar) {
  std::string const queryString =
      "FOR s IN testView0 LET kk = NOOPT(s.key) "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(1, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterListWithVar) {
  std::string const queryString =
      "FOR s IN 1..2 LET kk = s "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(1, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(1, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterList) {
  std::string const queryString =
      "FOR s IN 1..2 "
      "FOR d IN testView1 SEARCH d.key == s RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(1, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(1, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterListNonVolatile) {
  std::string const queryString =
      "FOR s IN 1..20 LET kk = NOEVAL(1) "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(1, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(20, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterQueryNonVolatile) {
  std::string const queryString =
      "FOR s IN testView0 COLLECT WITH COUNT INTO c LET kk = NOEVAL(c) "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(0, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilityFilterListSubquery) {
  std::string const queryString =
      "FOR s IN 1..20 LET kk = (FOR v IN testView0 SEARCH v.key == 1 RETURN "
      "v.key)[0] "
      "FOR d IN testView1 SEARCH d.key == kk RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(20, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilitySortFilterListSubquery) {
  std::string const queryString =
      "FOR s IN 1..20 LET kk = (FOR v IN testView0 SEARCH v.key == 1 RETURN "
      "v.key)[0] "
      "FOR d IN testView1 SEARCH d.key == kk SORT BM25(d, kk) RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_TRUE(view->volatility().first);
      ASSERT_TRUE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(20, res.data->slice().length());
}

TEST_F(IResearchViewVolatitlityTest, volatilitySortNonVolatileFilter) {
  std::string const queryString =
      "FOR s IN 1..20 LET kk = (FOR v IN testView0 SEARCH v.key == 1 RETURN "
      "v.key)[0] "
      "FOR d IN testView1 SEARCH d.key == 1 SORT BM25(d, kk) RETURN d";
  auto prepared = arangodb::tests::prepareQuery(*vocbase, queryString);
  auto plan = prepared->plan();
  ASSERT_NE(nullptr, plan);
  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, {arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW}, true);
  ASSERT_EQ(2, nodes.size());
  for (auto const* n : nodes) {
    arangodb::iresearch::IResearchViewNode const* view =
        static_cast<arangodb::iresearch::IResearchViewNode const*>(n);
    if (view->view()->name() == "testView1") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_TRUE(view->volatility().second);
    } else if (view->view()->name() == "testView0") {
      ASSERT_FALSE(view->volatility().first);
      ASSERT_FALSE(view->volatility().second);
    } else {
      ASSERT_TRUE(false);  // unexpected node
    }
  }
  // check volatility affects results
  auto res = arangodb::tests::executeQuery(*vocbase, queryString);

  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.data->slice().isArray());
  EXPECT_EQ(20, res.data->slice().length());
}

class IResearchViewBlockTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;

  IResearchViewBlockTest() : server(false) {
    arangodb::tests::init(true);

    server.addFeature<arangodb::FlushFeature>(false);
    server.startFeatures();

    auto& dbPathFeature = server.getFeature<arangodb::DatabasePathFeature>();
    arangodb::tests::setDatabasePath(
        dbPathFeature);  // ensure test data is stored in a unique directory

    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
    auto vocbase =
        dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
    std::shared_ptr<arangodb::LogicalCollection> collection0;
    {
      auto createJson = arangodb::velocypack::Parser::fromJson(
          "{ \"name\": \"testCollection0\", \"id\" : \"42\" }");
      collection0 = vocbase->createCollection(createJson->slice());
      EXPECT_NE(nullptr, collection0);
    }
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto logicalView = vocbase->createView(createJson->slice(), false);
    EXPECT_NE(nullptr, logicalView);
    auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": {"
        "\"testCollection0\": { \"includeAllFields\": true, "
        "\"trackListPositions\": true }"
        "}}");
    EXPECT_TRUE(logicalView->properties(updateJson->slice(), true, true).ok());
    std::vector<std::string> EMPTY_VECTOR;
    auto trx = std::make_shared<arangodb::transaction::Methods>(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        EMPTY_VECTOR, std::vector<std::string>{collection0->name()},
        EMPTY_VECTOR, arangodb::transaction::Options());

    EXPECT_TRUE(trx->begin().ok());
    // Fill dummy data in index only (to simulate some documents where already
    // removed from collection)
    arangodb::iresearch::IResearchLinkMeta meta;
    meta._includeAllFields = true;
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      auto indexes = collection0->getPhysical()->getAllIndexes();
      EXPECT_FALSE(indexes.empty());
      auto* l = static_cast<arangodb::iresearch::IResearchLinkMock*>(
          indexes[0].get());
      for (size_t i = 2; i < 10; ++i) {
        l->insert(*trx, arangodb::LocalDocumentId(i), doc->slice());
      }
    }
    // in collection only one alive doc
    auto aliveDoc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
    arangodb::OperationOptions options;
    EXPECT_TRUE(
        trx->insert(collection0->name(), aliveDoc->slice(), options).ok());
    EXPECT_TRUE(trx->commit().ok());
  }
};

TEST_F(IResearchViewBlockTest, retrieveWithMissingInCollectionUnordered) {
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  auto queryResult = arangodb::tests::executeQuery(
      *vocbase, "FOR d IN testView OPTIONS { waitForSync: true } RETURN d");
  ASSERT_TRUE(queryResult.result.ok());
  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());
  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(1, resultIt.size());
}

TEST_F(IResearchViewBlockTest, retrieveWithMissingInCollection) {
  auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
  auto vocbase = dbFeature.useDatabase(arangodb::StaticStrings::SystemDatabase);
  auto queryResult = arangodb::tests::executeQuery(
      *vocbase,
      "FOR d IN testView  OPTIONS { waitForSync: true } SORT BM25(d) RETURN d");
  ASSERT_TRUE(queryResult.result.ok());
  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());
  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(1, resultIt.size());
}
