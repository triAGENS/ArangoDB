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

#include "../Mocks/StorageEngineMock.h"
#include "../Mocks/Servers.h"
#include "common.h"
#include "gtest/gtest.h"

#include "Agency/AgencyFeature.h"
#include "Agency/Store.h"
#include "ApplicationFeatures/CommunicationFeaturePhase.h"
#include "ApplicationFeatures/GreetingsFeaturePhase.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AstNode.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/IResearchViewNode.h"
#include "Aql/ExecutionNode/SingletonNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Executor/NoResultsExecutor.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/SortCondition.h"
#include "Async/async.h"
#include "Auth/UserManager.h"
#include "Basics/ArangoGlobalContext.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/files.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "ClusterEngine/ClusterEngine.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "FeaturePhases/DatabaseFeaturePhase.h"
#ifdef USE_V8
#include "FeaturePhases/V8FeaturePhase.h"
#endif
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkCoordinator.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchViewCoordinator.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "Random/RandomFeature.h"
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
#include "Utils/ExecContext.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Indexes.h"
#include "velocypack/Iterator.h"
#include "velocypack/Parser.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchViewCoordinatorTest : public ::testing::Test {
 protected:
  arangodb::tests::mocks::MockCoordinator server;
  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor resourceMonitor{global};

  IResearchViewCoordinatorTest() : server("CRDN_0001") {
    arangodb::tests::init();
    TransactionStateMock::abortTransactionCount = 0;
    TransactionStateMock::beginTransactionCount = 0;
    TransactionStateMock::commitTransactionCount = 0;
  }

  void createTestDatabase(TRI_vocbase_t*& vocbase) {
    vocbase = server.createDatabase("testDatabase");
    ASSERT_NE(nullptr, vocbase);
    ASSERT_EQ("testDatabase", vocbase->name());
  }

  ~IResearchViewCoordinatorTest() = default;
};

#ifdef USE_ENTERPRISE
static constexpr size_t kEnterpriseFields = 1;
#else
static constexpr size_t kEnterpriseFields = 0;
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchViewCoordinatorTest, test_type) {
  static_assert(arangodb::iresearch::IResearchViewCoordinator::typeInfo() ==
                std::pair{arangodb::ViewType::kArangoSearch,
                          std::string_view{"arangosearch"}});
}

TEST_F(IResearchViewCoordinatorTest, test_rename) {
  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": \"1\", "
      "\"collections\": [1,2,3] }");

  Vocbase vocbase(testDBInfo(server.server()));
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(
      (arangodb::LogicalView::instantiate(view, vocbase, json->slice(), false)
           .ok()));
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(1 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(&vocbase == &view->vocbase());

  auto const res = view->rename("otherName");
  EXPECT_TRUE(res.fail());
  EXPECT_TRUE(TRI_ERROR_CLUSTER_UNSUPPORTED == res.errorNumber());
}

TEST_F(IResearchViewCoordinatorTest, visit_collections) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  std::string collectionId0("100");
  std::string collectionId1("101");
  std::string collectionId2("102");
  std::string viewId("1");
  auto collectionJson0 = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection0\", \"shards\":{} }");
  auto collectionJson1 = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection1\", \"shards\":{} }");
  auto collectionJson2 = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection2\", \"shards\":{} }");
  auto linkJson = arangodb::velocypack::Parser::fromJson("{ \"view\": \"1\" }");
  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": \"1\" }");

  ASSERT_TRUE(ci.createCollectionCoordinator(
                    vocbase->name(), collectionId0, 0, 1, 1, false,
                    collectionJson0->slice(), 0.0, false, nullptr,
                    arangodb::replication::Version::ONE)
                  .ok());

  auto logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
  ASSERT_TRUE((false == !logicalCollection0));
  ASSERT_TRUE((ci.createCollectionCoordinator(
                     vocbase->name(), collectionId1, 0, 1, 1, false,
                     collectionJson1->slice(), 0.0, false, nullptr,
                     arangodb::replication::Version::ONE)
                   .ok()));

  auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
  ASSERT_TRUE((false == !logicalCollection1));
  ASSERT_TRUE((ci.createCollectionCoordinator(
                     vocbase->name(), collectionId2, 0, 1, 1, false,
                     collectionJson2->slice(), 0.0, false, nullptr,
                     arangodb::replication::Version::ONE)
                   .ok()));

  auto logicalCollection2 = ci.getCollection(vocbase->name(), collectionId2);
  ASSERT_TRUE((false == !logicalCollection2));
  ASSERT_TRUE(
      (ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok()));

  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_TRUE((false == !logicalView));
  auto* view = dynamic_cast<arangodb::iresearch::IResearchViewCoordinator*>(
      logicalView.get());

  ASSERT_TRUE(nullptr != view);
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(1 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  std::shared_ptr<arangodb::Index> link;
  auto& factory = server.getFeature<arangodb::iresearch::IResearchFeature>()
                      .factory<arangodb::ClusterEngine>();
  EXPECT_NE(nullptr, factory.instantiate(*logicalCollection0, linkJson->slice(),
                                         arangodb::IndexId{1}, false));
  EXPECT_NE(nullptr, factory.instantiate(*logicalCollection1, linkJson->slice(),
                                         arangodb::IndexId{2}, false));
  EXPECT_NE(nullptr, factory.instantiate(*logicalCollection2, linkJson->slice(),
                                         arangodb::IndexId{3}, false));

  // visit view
  arangodb::DataSourceId expectedCollections[] = {arangodb::DataSourceId{1},
                                                  arangodb::DataSourceId{2},
                                                  arangodb::DataSourceId{3}};
  auto* begin = expectedCollections;
  EXPECT_TRUE(true ==
              view->visitCollections([&begin](arangodb::DataSourceId cid,
                                              arangodb::LogicalView::Indexes*) {
                *begin++ = cid;
                return true;
              }));
  EXPECT_TRUE(3 == (begin - expectedCollections));
}

TEST_F(IResearchViewCoordinatorTest, test_defaults) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // view definition with LogicalView (for persistence)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": \"1\" "
        "}");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE(
        (arangodb::LogicalView::instantiate(view, vocbase, json->slice(), false)
             .ok()));
    EXPECT_TRUE((nullptr != view));
    EXPECT_TRUE(
        (nullptr != std::dynamic_pointer_cast<
                        arangodb::iresearch::IResearchViewCoordinator>(view)));
    EXPECT_TRUE(("testView" == view->name()));
    EXPECT_TRUE((false == view->deleted()));
    EXPECT_TRUE((1 == view->id().id()));
    EXPECT_TRUE((arangodb::ViewType::kArangoSearch == view->type()));
    EXPECT_TRUE(
        (arangodb::LogicalDataSource::Category::kView == view->category()));
    EXPECT_TRUE((&vocbase == &view->vocbase()));

    // visit default view
    EXPECT_TRUE(
        (true == view->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // for persistence
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());
      builder.close();
      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      EXPECT_EQ(18 + kEnterpriseFields, slice.length());
      EXPECT_TRUE(
          (slice.hasKey("globallyUniqueId") &&
           slice.get("globallyUniqueId").isString() &&
           false == slice.get("globallyUniqueId").copyString().empty()));
      EXPECT_TRUE((slice.get("id").copyString() == "1"));
      EXPECT_TRUE((slice.hasKey("isSystem") &&
                   slice.get("isSystem").isBoolean() &&
                   false == slice.get("isSystem").getBoolean()));
      EXPECT_TRUE((slice.get("name").copyString() == "testView"));
      EXPECT_TRUE((slice.get("type").copyString() ==
                   arangodb::iresearch::StaticStrings::ViewArangoSearchType));
      EXPECT_TRUE((slice.hasKey("planId")));
      EXPECT_TRUE((false == slice.get("deleted").getBool()));
      EXPECT_TRUE((!slice.hasKey("links")));  // for persistence so no links
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }

    // properties
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();
      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE(
          (slice.hasKey("globallyUniqueId") &&
           slice.get("globallyUniqueId").isString() &&
           false == slice.get("globallyUniqueId").copyString().empty()));
      EXPECT_TRUE((slice.get("id").copyString() == "1"));
      EXPECT_TRUE((slice.get("name").copyString() == "testView"));
      EXPECT_TRUE((slice.get("type").copyString() ==
                   arangodb::iresearch::StaticStrings::ViewArangoSearchType));
      EXPECT_TRUE((!slice.hasKey("planId")));
      EXPECT_TRUE((!slice.hasKey("deleted")));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   0 == slice.get("links").length()));
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }

    // list
    {
      arangodb::velocypack::Builder builder;
      builder.openObject();
      auto res = view->properties(
          builder, arangodb::LogicalDataSource::Serialization::List);
      ASSERT_TRUE(res.ok());
      builder.close();
      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      EXPECT_TRUE((4U == slice.length()));
      EXPECT_TRUE(
          (slice.hasKey("globallyUniqueId") &&
           slice.get("globallyUniqueId").isString() &&
           false == slice.get("globallyUniqueId").copyString().empty()));
      EXPECT_TRUE((slice.get("id").copyString() == "1"));
      EXPECT_TRUE((slice.get("name").copyString() == "testView"));
      EXPECT_TRUE((slice.get("type").copyString() ==
                   arangodb::iresearch::StaticStrings::ViewArangoSearchType));
      EXPECT_TRUE((!slice.hasKey("planId")));
      EXPECT_TRUE((!slice.hasKey("deleted")));
      EXPECT_TRUE((!slice.hasKey("properties")));
    }
  }

  // new view definition with links to missing collections
  {
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\", \"links\": { \"testCollection\": {} } }");
    auto viewId = "testView";

    arangodb::LogicalView::ptr logicalView;
    auto res = arangodb::iresearch::IResearchViewCoordinator::factory().create(
        logicalView, *vocbase, viewCreateJson->slice(), true);
    EXPECT_TRUE((TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND == res.errorNumber()));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((true == !logicalView));
  }

  // new view definition with links with invalid definition
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\", \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\", \"links\": { \"testCollection\": 42 } }");
    auto collectionId = std::to_string(1);
    auto viewId = "testView";

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    arangodb::LogicalView::ptr logicalView;
    auto res = arangodb::iresearch::IResearchViewCoordinator::factory().create(
        logicalView, *vocbase, viewCreateJson->slice(), true);
    EXPECT_TRUE((TRI_ERROR_BAD_PARAMETER == res.errorNumber()));

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((true == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
  }

  // new view definition with links (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\", \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\", \"links\": { \"testCollection\": {} } }");
    auto collectionId = std::to_string(1);
    auto viewId = "testView";

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    arangodb::LogicalView::ptr logicalView;
    auto res = arangodb::iresearch::IResearchViewCoordinator::factory().create(
        logicalView, *vocbase, viewCreateJson->slice(), true);
    EXPECT_TRUE((TRI_ERROR_FORBIDDEN == res.errorNumber()));

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((true == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
  }

  // new view definition with links
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\", \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\", \"links\": { \"testCollection\": {} } }");
    auto collectionId = std::to_string(1);
    auto viewId = "testView";

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });

    // simulate heartbeat thread (create index in current)
    {
      auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                        std::to_string(logicalCollection->id().id());
      auto const value = arangodb::velocypack::Parser::fromJson(
          "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
      EXPECT_TRUE((arangodb::AgencyComm(server.server())
                       .setValue(path, value->slice(), 0.0)
                       .successful()));
    }

    arangodb::LogicalView::ptr logicalView;
    EXPECT_TRUE(
        (arangodb::iresearch::IResearchViewCoordinator::factory()
             .create(logicalView, *vocbase, viewCreateJson->slice(), false)
             .ok()));

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (false == logicalView->visitCollections(
                      [](arangodb::DataSourceId,
                         arangodb::LogicalView::Indexes*) { return false; })));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_create_drop_view) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // no name specified
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(ci.uniqid());
    EXPECT_TRUE(
        (TRI_ERROR_BAD_PARAMETER ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
  }

  // empty name
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"\", \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(ci.uniqid());
    EXPECT_TRUE(
        (TRI_ERROR_BAD_PARAMETER ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
  }

  // wrong name
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": 5, \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(ci.uniqid());
    EXPECT_TRUE(
        (TRI_ERROR_BAD_PARAMETER ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
  }

  // no type specified
  {
    auto json =
        arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    auto viewId = std::to_string(ci.uniqid());
    EXPECT_TRUE(
        (TRI_ERROR_BAD_PARAMETER ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
  }

  // create and drop view (no id specified)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID

    EXPECT_TRUE((
        ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok()));

    // get current plan version
    auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    auto view = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(viewId == std::to_string(view->id().id()));
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // create duplicate view
    EXPECT_TRUE(
        (TRI_ERROR_ARANGO_DUPLICATE_NAME ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
    EXPECT_TRUE(planVersion ==
                arangodb::tests::getCurrentPlanVersion(server.server()));
    EXPECT_TRUE(view == ci.getView(vocbase->name(), view->name()));

    // drop view
    EXPECT_TRUE(view->drop().ok());
    EXPECT_TRUE(planVersion <
                arangodb::tests::getCurrentPlanVersion(server.server()));

    // check there is no more view
    ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));

    // drop already dropped view
    EXPECT_TRUE((view->drop().ok()));
  }

  // create and drop view
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
        "}");
    auto viewId = std::to_string(42);

    EXPECT_TRUE((
        ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok()));
    EXPECT_TRUE("42" == viewId);

    // get current plan version
    auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    auto view = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(42 == view->id().id());
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // create duplicate view
    EXPECT_TRUE(
        (TRI_ERROR_ARANGO_DUPLICATE_NAME ==
         ci.createViewCoordinator(vocbase->name(), viewId, json->slice())
             .errorNumber()));
    EXPECT_TRUE(planVersion ==
                arangodb::tests::getCurrentPlanVersion(server.server()));
    EXPECT_TRUE(view == ci.getView(vocbase->name(), view->name()));

    // drop view
    EXPECT_TRUE(view->drop().ok());
    EXPECT_TRUE(planVersion <
                arangodb::tests::getCurrentPlanVersion(server.server()));

    // check there is no more view
    ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));

    // drop already dropped view
    EXPECT_TRUE((view->drop().ok()));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_create_link_in_background) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
      "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": { \"testCollection\": { \"includeAllFields\":true, "
      "\"inBackground\":true } } }");
  auto collectionId = std::to_string(1);
  auto viewId = std::to_string(42);

  ASSERT_TRUE((ci.createCollectionCoordinator(
                     vocbase->name(), collectionId, 0, 1, 1, false,
                     collectionJson->slice(), 0.0, false, nullptr,
                     arangodb::replication::Version::ONE)
                   .ok()));
  auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
  ASSERT_NE(nullptr, logicalCollection);
  ASSERT_TRUE((
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok()));
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  ASSERT_TRUE(logicalCollection->getPhysical()->getAllIndexes().empty());
  ASSERT_NE(nullptr, ci.getView(vocbase->name(), viewId));

  //  link creation
  {
    // simulate heartbeat thread (create index in current)
    {
      auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                        std::to_string(logicalCollection->id().id());
      auto const value = arangodb::velocypack::Parser::fromJson(
          "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(path, value->slice(), 0.0)
                      .successful());
    }
    ASSERT_TRUE(
        logicalView->properties(viewUpdateJson->slice(), true, true).ok());
  }
  // check agency record
  {
    VPackBuilder agencyRecord;
    agencyRecord.openArray();

    server.getFeature<arangodb::ClusterFeature>().agencyCache().store().read(
        arangodb::velocypack::Parser::fromJson(
            "[\"arango/Plan/Collections/testDatabase/" + collectionId + "\"]")
            ->slice(),
        agencyRecord);
    agencyRecord.close();

    ASSERT_TRUE(agencyRecord.slice().isArray());
    auto collectionInfoSlice = agencyRecord.slice().at(0);
    auto indexesSlice = collectionInfoSlice.get("arango")
                            .get("Plan")
                            .get("Collections")
                            .get("testDatabase")
                            .get(collectionId)
                            .get("indexes");
    ASSERT_TRUE(indexesSlice.isArray());
    auto linkSlice = indexesSlice.at(0);
    ASSERT_TRUE(linkSlice.hasKey("inBackground"));
    ASSERT_TRUE(linkSlice.get("inBackground").isBool());
    ASSERT_TRUE(linkSlice.get("inBackground").getBool());
  }
  // check index definition in collection
  {
    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_NE(nullptr, logicalCollection);
    auto indexes = logicalCollection->getPhysical()->getAllIndexes();
    ASSERT_EQ(1, indexes.size());  // arangosearch should be there
    auto index = indexes[0];
    ASSERT_EQ(arangodb::Index::TRI_IDX_TYPE_IRESEARCH_LINK, index->type());
    VPackBuilder builder;
    index->toVelocyPack(builder, arangodb::Index::makeFlags(
                                     arangodb::Index::Serialize::Internals));
    // temporary property should not be returned
    ASSERT_FALSE(builder.slice().hasKey("inBackground"));
  }

  // Check link definition in view
  {
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_NE(nullptr, logicalView);
    VPackBuilder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();
    ASSERT_TRUE(builder.slice().hasKey("links"));
    auto links = builder.slice().get("links");
    ASSERT_TRUE(links.isObject());
    ASSERT_TRUE(links.hasKey("testCollection"));
    auto testCollectionSlice = links.get("testCollection");
    // temporary property should not be returned
    ASSERT_FALSE(testCollectionSlice.hasKey("inBackground"));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_drop_with_link) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
      "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": { \"testCollection\": {} } }");
  auto collectionId = std::to_string(1);
  auto viewId = std::to_string(42);

  EXPECT_TRUE((ci.createCollectionCoordinator(
                     vocbase->name(), collectionId, 0, 1, 1, false,
                     collectionJson->slice(), 0.0, false, nullptr,
                     arangodb::replication::Version::ONE)
                   .ok()));
  auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
  ASSERT_TRUE((false == !logicalCollection));
  EXPECT_TRUE((
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok()));
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_TRUE((false == !logicalView));

  EXPECT_TRUE(
      (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
  EXPECT_TRUE((false == !ci.getView(vocbase->name(), viewId)));

  // initial link creation
  {
    // simulate heartbeat thread (create index in current)
    {
      auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                        std::to_string(logicalCollection->id().id());
      auto const value = arangodb::velocypack::Parser::fromJson(
          "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(path, value->slice(), 0.0)
                      .successful());
    }

    EXPECT_TRUE(
        (logicalView->properties(viewUpdateJson->slice(), true, true).ok()));
    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (false == logicalView->visitCollections(
                      [](arangodb::DataSourceId,
                         arangodb::LogicalView::Indexes*) { return false; })));

    // simulate heartbeat thread (remove index in current)
    {
      auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                        std::to_string(logicalCollection->id().id()) +
                        "/s12345/indexes";
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .removeValues(path, false)
                      .successful());
    }
  }

  {
    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // not authorised (NONE collection) as per
    // https://github.com/arangodb/backlog/issues/459
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::NONE);   // for missing collections
                                          // User::collectionAuthLevel(...)
                                          // returns database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE((TRI_ERROR_FORBIDDEN == logicalView->drop().errorNumber()));
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false == !ci.getView(vocbase->name(), viewId)));
    }

    // authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE((true == logicalView->drop().ok()));
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      EXPECT_TRUE(
          (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((true == !ci.getView(vocbase->name(), viewId)));
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_properties) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create view
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID

    EXPECT_TRUE((
        ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok()));

    // get current plan version
    auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    auto view = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(viewId == std::to_string(view->id().id()));
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // check default properties
    {
      VPackBuilder builder;
      builder.openObject();
      auto res = view->properties(
          builder, arangodb::LogicalDataSource::Serialization::Properties);
      ASSERT_TRUE(res.ok());
      builder.close();

      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;
      EXPECT_TRUE(meta.init(builder.slice(), error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(meta == arangodb::iresearch::IResearchViewMeta::DEFAULT());
    }

    decltype(view) fullyUpdatedView;

    // update properties - full update
    {
      auto props = arangodb::velocypack::Parser::fromJson(
          "{ \"cleanupIntervalStep\": 42, \"consolidationIntervalMsec\": 50 }");
      EXPECT_TRUE(view->properties(props->slice(), true, false).ok());
      EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                    server.server()));  // plan version changed
      planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

      fullyUpdatedView = ci.getView(vocbase->name(), viewId);
      EXPECT_TRUE(fullyUpdatedView != view);  // different objects
      ASSERT_TRUE(nullptr != fullyUpdatedView);
      ASSERT_TRUE(nullptr != std::dynamic_pointer_cast<
                                 arangodb::iresearch::IResearchViewCoordinator>(
                                 fullyUpdatedView));
      EXPECT_TRUE("testView" == fullyUpdatedView->name());
      EXPECT_TRUE(false == fullyUpdatedView->deleted());
      EXPECT_TRUE(viewId == std::to_string(fullyUpdatedView->id().id()));
      EXPECT_TRUE(arangodb::ViewType::kArangoSearch ==
                  fullyUpdatedView->type());
      EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                  fullyUpdatedView->category());
      EXPECT_TRUE(vocbase == &fullyUpdatedView->vocbase());

      // check recently updated properties
      {
        VPackBuilder builder;
        builder.openObject();
        auto res = fullyUpdatedView->properties(
            builder, arangodb::LogicalDataSource::Serialization::Properties);
        ASSERT_TRUE(res.ok());
        builder.close();

        arangodb::iresearch::IResearchViewMeta meta;
        arangodb::iresearch::IResearchViewMeta expected;
        expected._cleanupIntervalStep = 42;
        expected._consolidationIntervalMsec = 50;
        std::string error;
        EXPECT_TRUE(meta.init(builder.slice(), error));
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(expected == meta);
      }

      // old object remains the same
      {
        VPackBuilder builder;
        builder.openObject();
        auto res = view->properties(
            builder, arangodb::LogicalDataSource::Serialization::Properties);
        ASSERT_TRUE(res.ok());
        builder.close();

        arangodb::iresearch::IResearchViewMeta meta;
        std::string error;
        EXPECT_TRUE(meta.init(builder.slice(), error));
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(meta == arangodb::iresearch::IResearchViewMeta::DEFAULT());
      }
    }

    // partially update properties
    {
      auto props = arangodb::velocypack::Parser::fromJson(
          "{ \"consolidationIntervalMsec\": 42 }");
      EXPECT_TRUE(
          fullyUpdatedView->properties(props->slice(), true, true).ok());
      EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                    server.server()));  // plan version changed
      planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

      auto partiallyUpdatedView = ci.getView(vocbase->name(), viewId);
      EXPECT_TRUE(partiallyUpdatedView != view);  // different objects
      ASSERT_TRUE(nullptr != partiallyUpdatedView);
      ASSERT_TRUE(nullptr != std::dynamic_pointer_cast<
                                 arangodb::iresearch::IResearchViewCoordinator>(
                                 partiallyUpdatedView));
      EXPECT_TRUE("testView" == partiallyUpdatedView->name());
      EXPECT_TRUE(false == partiallyUpdatedView->deleted());
      EXPECT_TRUE(viewId == std::to_string(partiallyUpdatedView->id().id()));
      EXPECT_TRUE(arangodb::ViewType::kArangoSearch ==
                  partiallyUpdatedView->type());
      EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                  partiallyUpdatedView->category());
      EXPECT_TRUE(vocbase == &partiallyUpdatedView->vocbase());

      // check recently updated properties
      {
        VPackBuilder builder;
        builder.openObject();
        auto res = partiallyUpdatedView->properties(
            builder, arangodb::LogicalDataSource::Serialization::Properties);
        ASSERT_TRUE(res.ok());
        builder.close();

        arangodb::iresearch::IResearchViewMeta meta;
        arangodb::iresearch::IResearchViewMeta expected;
        expected._cleanupIntervalStep = 42;
        expected._consolidationIntervalMsec = 42;
        std::string error;
        EXPECT_TRUE(meta.init(builder.slice(), error));
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(expected == meta);
      }
    }

    // drop view
    EXPECT_TRUE(view->drop().ok());
    EXPECT_TRUE(planVersion <
                arangodb::tests::getCurrentPlanVersion(server.server()));

    // there is no more view
    ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_properties_user_request) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  {
    auto const collectionId = "100";
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"100\", \"planId\": \"100\",  \"name\": "
        "\"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE(ci.createCollectionCoordinator(
                      vocbase->name(), collectionId, 0, 1, 1, false,
                      collectionJson->slice(), 0.0, false, nullptr,
                      arangodb::replication::Version::ONE)
                    .ok());

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_NE(nullptr, logicalCollection);
  }

  // new view definition with links
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101 }");

  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{  \"links\": { "
      "    \"testCollection\": { "
      "      \"id\":1, "
      "      \"includeAllFields\":true, "
      "      \"analyzers\": [\"inPlace\"], "
      "      \"analyzerDefinitions\": [ { \"name\" : \"inPlace\", "
      "\"type\":\"identity\", \"properties\":{}, \"features\":[] } ],"
      "      \"storedValues\":[[], [\"\"], [\"\"], [\"test.t\"], "
      "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"none\"}] "
      "    } "
      "  } }");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID
  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok());
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // simulate heartbeat thread (create index in current)
  {
    auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                      std::to_string(logicalCollection->id().id());
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(path, value->slice(), 0.0)
                    .successful());
  }

  ASSERT_TRUE(
      logicalView->properties(viewUpdateJson->slice(), true, false).ok());
  logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // check serialization for listing
  {
    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::List);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(4, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
  }

  // check serialization for properties
  {
    VPackSlice tmpSlice, tmpSlice2;

    VPackBuilder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(5, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
    }
  }

  // check serialization for persistence
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Persistence);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(18 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("planId").isString() &&
                "101" == slice.get("planId").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("deleted").isBool() &&
                !slice.get("deleted").getBool());
    EXPECT_TRUE(slice.get("isSystem").isBool() &&
                !slice.get("isSystem").getBool());

    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("version");
    EXPECT_TRUE(tmpSlice.isNumber<uint32_t>() &&
                1 == tmpSlice.getNumber<uint32_t>());
  }

  // check serialization for inventory
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Inventory);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(11 + kEnterpriseFields, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
      EXPECT_TRUE(tmpSlice2.get("version").isNumber());
      EXPECT_EQ(1, tmpSlice2.get("version").getNumber<uint32_t>());
      EXPECT_TRUE(tmpSlice2.get("collectionName").isString() &&
                  "testCollection" ==
                      tmpSlice2.get("collectionName").copyString());
      EXPECT_TRUE(tmpSlice2.get("primarySortCompression").isString() &&
                  "lz4" ==
                      tmpSlice2.get("primarySortCompression").copyString());
      EXPECT_TRUE(tmpSlice2.get("storedValues").isArray() &&
                  0 == tmpSlice2.get("storedValues").length());
      EXPECT_TRUE(tmpSlice2.get("primarySort").isArray());
      auto valueTopK = tmpSlice2.get("optimizeTopK");
#ifdef USE_ENTERPRISE
      EXPECT_TRUE(valueTopK.isEmptyArray());
#else
      EXPECT_TRUE(valueTopK.isNone());
#endif

      tmpSlice2 = tmpSlice2.get("analyzerDefinitions");
      ASSERT_TRUE(tmpSlice2.isArray());
      ASSERT_EQ(1, tmpSlice2.length());
      tmpSlice2 = tmpSlice2.at(0);
      ASSERT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(4, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("name").isString() &&
                  "inPlace" == tmpSlice2.get("name").copyString());
      EXPECT_TRUE(tmpSlice2.get("type").isString() &&
                  "identity" == tmpSlice2.get("type").copyString());
      EXPECT_TRUE(tmpSlice2.get("properties").isObject() &&
                  0 == tmpSlice2.get("properties").length());
      EXPECT_TRUE(tmpSlice2.get("features").isArray() &&
                  0 == tmpSlice2.get("features").length());
    }
  }
}

TEST_F(IResearchViewCoordinatorTest,
       test_properties_user_request_explicit_version) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  {
    auto const collectionId = "100";
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"100\", \"planId\": \"100\",  \"name\": "
        "\"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE(ci.createCollectionCoordinator(
                      vocbase->name(), collectionId, 0, 1, 1, false,
                      collectionJson->slice(), 0.0, false, nullptr,
                      arangodb::replication::Version::ONE)
                    .ok());

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_NE(nullptr, logicalCollection);
  }

  // new view definition with links
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101 }");

  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{  \"links\": { "
      "    \"testCollection\": { "
      "      \"id\":1, "
      "      \"includeAllFields\":true, "
      "      \"version\":0, "
      "      \"analyzers\": [\"inPlace\"], "
      "      \"analyzerDefinitions\": [ { \"name\" : \"inPlace\", "
      "\"type\":\"identity\", \"properties\":{}, \"features\":[] } ],"
      "      \"storedValues\":[[], [\"\"], [\"\"], [\"test.t\"], "
      "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"none\"}] "
      "    } "
      "  } }");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID
  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok());
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // simulate heartbeat thread (create index in current)
  {
    auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                      std::to_string(logicalCollection->id().id());
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(path, value->slice(), 0.0)
                    .successful());
  }

  ASSERT_TRUE(
      logicalView->properties(viewUpdateJson->slice(), true, false).ok());
  logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // check serialization for listing
  {
    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::List);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(4, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
  }

  // check serialization for properties
  {
    VPackSlice tmpSlice, tmpSlice2;

    VPackBuilder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(5, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
    }
  }

  // check serialization for persistence
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Persistence);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(18 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("planId").isString() &&
                "101" == slice.get("planId").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("deleted").isBool() &&
                !slice.get("deleted").getBool());
    EXPECT_TRUE(slice.get("isSystem").isBool() &&
                !slice.get("isSystem").getBool());

    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("version");
    EXPECT_TRUE(tmpSlice.isNumber<uint32_t>() &&
                1 == tmpSlice.getNumber<uint32_t>());
  }

  // check serialization for inventory
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Inventory);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(11 + kEnterpriseFields, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
      EXPECT_TRUE(tmpSlice2.get("version").isNumber());
      EXPECT_EQ(0, tmpSlice2.get("version").getNumber<uint32_t>());
      EXPECT_TRUE(tmpSlice2.get("collectionName").isString() &&
                  "testCollection" ==
                      tmpSlice2.get("collectionName").copyString());
      EXPECT_TRUE(tmpSlice2.get("primarySortCompression").isString() &&
                  "lz4" ==
                      tmpSlice2.get("primarySortCompression").copyString());
      EXPECT_TRUE(tmpSlice2.get("storedValues").isArray() &&
                  0 == tmpSlice2.get("storedValues").length());
      EXPECT_TRUE(tmpSlice2.get("primarySort").isArray());
      auto valueTopK = tmpSlice2.get("optimizeTopK");
#ifdef USE_ENTERPRISE
      EXPECT_TRUE(valueTopK.isEmptyArray());
#else
      EXPECT_TRUE(valueTopK.isNone());
#endif

      tmpSlice2 = tmpSlice2.get("analyzerDefinitions");
      ASSERT_TRUE(tmpSlice2.isArray());
      ASSERT_EQ(1, tmpSlice2.length());
      tmpSlice2 = tmpSlice2.at(0);
      ASSERT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(4, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("name").isString() &&
                  "inPlace" == tmpSlice2.get("name").copyString());
      EXPECT_TRUE(tmpSlice2.get("type").isString() &&
                  "identity" == tmpSlice2.get("type").copyString());
      EXPECT_TRUE(tmpSlice2.get("properties").isObject() &&
                  0 == tmpSlice2.get("properties").length());
      EXPECT_TRUE(tmpSlice2.get("features").isArray() &&
                  0 == tmpSlice2.get("features").length());
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, test_properties_internal_request) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  {
    auto const collectionId = "100";
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"100\", \"planId\": \"100\",  \"name\": "
        "\"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE(ci.createCollectionCoordinator(
                      vocbase->name(), collectionId, 0, 1, 1, false,
                      collectionJson->slice(), 0.0, false, nullptr,
                      arangodb::replication::Version::ONE)
                    .ok());

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_NE(nullptr, logicalCollection);
  }

  // new view definition with links
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101 }");

  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{  \"links\": { "
      "    \"testCollection\": { "
      "      \"id\":1, "
      "      \"includeAllFields\":true, "
      "      \"analyzers\": [\"inPlace\"], "
      "      \"analyzerDefinitions\": [ { \"name\" : \"inPlace\", "
      "\"type\":\"identity\", \"properties\":{}, \"features\":[] } ],"
      "      \"storedValues\":[[], [\"\"], [\"\"], [\"test.t\"], "
      "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"none\"}] "
      "    } "
      "  } }");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID
  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok());
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // simulate heartbeat thread (create index in current)
  {
    auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                      std::to_string(logicalCollection->id().id());
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(path, value->slice(), 0.0)
                    .successful());
  }

  ASSERT_TRUE(
      logicalView->properties(viewUpdateJson->slice(), false, false).ok());
  logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // check serialization for listing
  {
    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::List);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(4, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
  }

  // check serialization for properties
  {
    VPackSlice tmpSlice, tmpSlice2;

    VPackBuilder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(5, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
    }
  }

  // check serialization for persistence
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Persistence);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(18 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("planId").isString() &&
                "101" == slice.get("planId").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("deleted").isBool() &&
                !slice.get("deleted").getBool());
    EXPECT_TRUE(slice.get("isSystem").isBool() &&
                !slice.get("isSystem").getBool());

    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("version");
    EXPECT_TRUE(tmpSlice.isNumber<uint32_t>() &&
                1 == tmpSlice.getNumber<uint32_t>());
  }

  // check serialization for inventory
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Inventory);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(11 + kEnterpriseFields, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
      EXPECT_TRUE(tmpSlice2.get("version").isNumber());
      EXPECT_EQ(0, tmpSlice2.get("version").getNumber<uint32_t>());
      EXPECT_TRUE(tmpSlice2.get("collectionName").isString() &&
                  "testCollection" ==
                      tmpSlice2.get("collectionName").copyString());
      EXPECT_TRUE(tmpSlice2.get("primarySortCompression").isString() &&
                  "lz4" ==
                      tmpSlice2.get("primarySortCompression").copyString());
      EXPECT_TRUE(tmpSlice2.get("storedValues").isArray() &&
                  0 == tmpSlice2.get("storedValues").length());
      EXPECT_TRUE(tmpSlice2.get("primarySort").isArray());
      auto valueTopK = tmpSlice2.get("optimizeTopK");
#ifdef USE_ENTERPRISE
      EXPECT_TRUE(valueTopK.isEmptyArray());
#else
      EXPECT_TRUE(valueTopK.isNone());
#endif

      tmpSlice2 = tmpSlice2.get("analyzerDefinitions");
      ASSERT_TRUE(tmpSlice2.isArray());
      ASSERT_EQ(1, tmpSlice2.length());
      tmpSlice2 = tmpSlice2.at(0);
      ASSERT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(4, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("name").isString() &&
                  "inPlace" == tmpSlice2.get("name").copyString());
      EXPECT_TRUE(tmpSlice2.get("type").isString() &&
                  "identity" == tmpSlice2.get("type").copyString());
      EXPECT_TRUE(tmpSlice2.get("properties").isObject() &&
                  0 == tmpSlice2.get("properties").length());
      EXPECT_TRUE(tmpSlice2.get("features").isArray() &&
                  0 == tmpSlice2.get("features").length());
    }
  }
}

TEST_F(IResearchViewCoordinatorTest,
       test_properties_internal_request_explicit_version) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  {
    auto const collectionId = "100";
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"100\", \"planId\": \"100\",  \"name\": "
        "\"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE(ci.createCollectionCoordinator(
                      vocbase->name(), collectionId, 0, 1, 1, false,
                      collectionJson->slice(), 0.0, false, nullptr,
                      arangodb::replication::Version::ONE)
                    .ok());

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_NE(nullptr, logicalCollection);
  }

  // new view definition with links
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101 }");

  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{  \"links\": { "
      "    \"testCollection\": { "
      "      \"id\":1, "
      "      \"includeAllFields\":true, "
      "      \"version\":1, "
      "      \"analyzers\": [\"inPlace\"], "
      "      \"analyzerDefinitions\": [ { \"name\" : \"inPlace\", "
      "\"type\":\"identity\", \"properties\":{}, \"features\":[] } ],"
      "      \"storedValues\":[[], [\"\"], [\"\"], [\"test.t\"], "
      "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"none\"}] "
      "    } "
      "  } }");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID
  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, viewCreateJson->slice())
          .ok());
  auto logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // simulate heartbeat thread (create index in current)
  {
    auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                      std::to_string(logicalCollection->id().id());
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(path, value->slice(), 0.0)
                    .successful());
  }

  ASSERT_TRUE(
      logicalView->properties(viewUpdateJson->slice(), false, false).ok());
  logicalView = ci.getView(vocbase->name(), viewId);
  ASSERT_NE(nullptr, logicalView);

  // check serialization for listing
  {
    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::List);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(4, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
  }

  // check serialization for properties
  {
    VPackSlice tmpSlice, tmpSlice2;

    VPackBuilder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(5, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
    }
  }

  // check serialization for persistence
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Persistence);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(18 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("planId").isString() &&
                "101" == slice.get("planId").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("deleted").isBool() &&
                !slice.get("deleted").getBool());
    EXPECT_TRUE(slice.get("isSystem").isBool() &&
                !slice.get("isSystem").getBool());

    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("version");
    EXPECT_TRUE(tmpSlice.isNumber<uint32_t>() &&
                1 == tmpSlice.getNumber<uint32_t>());
  }

  // check serialization for inventory
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = logicalView->properties(
        builder, arangodb::LogicalDataSource::Serialization::Inventory);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("id").isString() &&
                "101" == slice.get("id").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                0 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
    {  // links
      tmpSlice = slice.get("links");
      EXPECT_TRUE(tmpSlice.isObject());
      EXPECT_EQ(1, tmpSlice.length());
      tmpSlice2 = tmpSlice.get("testCollection");
      EXPECT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(11 + kEnterpriseFields, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("analyzers").isArray() &&
                  1 == tmpSlice2.get("analyzers").length() &&
                  "inPlace" == tmpSlice2.get("analyzers").at(0).copyString());
      EXPECT_TRUE(tmpSlice2.get("fields").isObject() &&
                  0 == tmpSlice2.get("fields").length());
      EXPECT_TRUE(tmpSlice2.get("includeAllFields").isBool() &&
                  tmpSlice2.get("includeAllFields").getBool());
      EXPECT_TRUE(tmpSlice2.get("trackListPositions").isBool() &&
                  !tmpSlice2.get("trackListPositions").getBool());
      EXPECT_TRUE(tmpSlice2.get("storeValues").isString() &&
                  "none" == tmpSlice2.get("storeValues").copyString());
      EXPECT_TRUE(tmpSlice2.get("version").isNumber());
      EXPECT_EQ(1, tmpSlice2.get("version").getNumber<uint32_t>());
      EXPECT_TRUE(tmpSlice2.get("collectionName").isString() &&
                  "testCollection" ==
                      tmpSlice2.get("collectionName").copyString());
      EXPECT_TRUE(tmpSlice2.get("primarySortCompression").isString() &&
                  "lz4" ==
                      tmpSlice2.get("primarySortCompression").copyString());
      EXPECT_TRUE(tmpSlice2.get("storedValues").isArray() &&
                  0 == tmpSlice2.get("storedValues").length());
      EXPECT_TRUE(tmpSlice2.get("primarySort").isArray());
      auto valueTopK = tmpSlice2.get("optimizeTopK");
#ifdef USE_ENTERPRISE
      EXPECT_TRUE(valueTopK.isEmptyArray());
#else
      EXPECT_TRUE(valueTopK.isNone());
#endif

      tmpSlice2 = tmpSlice2.get("analyzerDefinitions");
      ASSERT_TRUE(tmpSlice2.isArray());
      ASSERT_EQ(1, tmpSlice2.length());
      tmpSlice2 = tmpSlice2.at(0);
      ASSERT_TRUE(tmpSlice2.isObject());
      EXPECT_EQ(4, tmpSlice2.length());
      EXPECT_TRUE(tmpSlice2.get("name").isString() &&
                  "inPlace" == tmpSlice2.get("name").copyString());
      EXPECT_TRUE(tmpSlice2.get("type").isString() &&
                  "identity" == tmpSlice2.get("type").copyString());
      EXPECT_TRUE(tmpSlice2.get("properties").isObject() &&
                  0 == tmpSlice2.get("properties").length());
      EXPECT_TRUE(tmpSlice2.get("features").isArray() &&
                  0 == tmpSlice2.get("features").length());
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, test_primary_compression_properties) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create view
  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", "
      "\"type\": \"arangosearch\", "
      "\"writebufferActive\": 25, "
      "\"writebufferIdle\": 64, "
      "\"locale\": \"C\", "
      "\"version\": 1, "
      "\"primarySortCompression\":\"none\","
      "\"primarySort\": [ "
      "{ \"field\": \"my.Nested.field\", \"direction\": \"asc\" }, "
      "{ \"field\": \"another.field\", \"asc\": false } "
      "]"
      "}");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID

  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok());

  auto view = ci.getView(vocbase->name(), viewId);
  EXPECT_NE(nullptr, view);
  EXPECT_NE(
      nullptr,
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  // check serialization for properties
  {
    VPackSlice tmpSlice, tmpSlice2;

    VPackBuilder builder;
    builder.openObject();
    auto res = view->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                25 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(2, tmpSlice.length());
    {
      auto first = tmpSlice.at(0);
      ASSERT_TRUE(first.isObject());
      tmpSlice2 = first.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("my.Nested.field", tmpSlice2.copyString());
      tmpSlice2 = first.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_TRUE(tmpSlice2.getBoolean());
    }
    {
      auto second = tmpSlice.at(1);
      ASSERT_TRUE(second.isObject());
      tmpSlice2 = second.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("another.field", tmpSlice2.copyString());
      tmpSlice2 = second.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_FALSE(tmpSlice2.getBoolean());
    }
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("none"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
  }

  // check serialization for persistence
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = view->properties(
        builder, arangodb::LogicalDataSource::Serialization::Persistence);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(18 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("deleted").isBool() &&
                !slice.get("deleted").getBool());
    EXPECT_TRUE(slice.get("isSystem").isBool() &&
                !slice.get("isSystem").getBool());

    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                25 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(2, tmpSlice.length());
    {
      auto first = tmpSlice.at(0);
      ASSERT_TRUE(first.isObject());
      tmpSlice2 = first.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("my.Nested.field", tmpSlice2.copyString());
      tmpSlice2 = first.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_TRUE(tmpSlice2.getBoolean());
    }
    {
      auto second = tmpSlice.at(1);
      ASSERT_TRUE(second.isObject());
      tmpSlice2 = second.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("another.field", tmpSlice2.copyString());
      tmpSlice2 = second.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_FALSE(tmpSlice2.getBoolean());
    }
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("none"), tmpSlice.copyString());
  }

  // check serialization for inventory
  {
    VPackSlice tmpSlice, tmpSlice2;

    arangodb::velocypack::Builder builder;
    builder.openObject();
    auto res = view->properties(
        builder, arangodb::LogicalDataSource::Serialization::Inventory);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.get("name").isString() &&
                "testView" == slice.get("name").copyString());
    EXPECT_TRUE(slice.get("type").isString() &&
                "arangosearch" == slice.get("type").copyString());
    EXPECT_TRUE(slice.get("globallyUniqueId").isString() &&
                !slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.get("consolidationIntervalMsec").isNumber() &&
                1000 ==
                    slice.get("consolidationIntervalMsec").getNumber<size_t>());
    EXPECT_TRUE(slice.get("cleanupIntervalStep").isNumber() &&
                2 == slice.get("cleanupIntervalStep").getNumber<size_t>());
    EXPECT_TRUE(slice.get("commitIntervalMsec").isNumber() &&
                1000 == slice.get("commitIntervalMsec").getNumber<size_t>());
    {  // consolidation policy
      tmpSlice = slice.get("consolidationPolicy");
      EXPECT_TRUE(tmpSlice.isObject() && 6 == tmpSlice.length());
      tmpSlice2 = tmpSlice.get("type");
      EXPECT_TRUE(tmpSlice2.isString() &&
                  std::string("tier") == tmpSlice2.copyString());
      tmpSlice2 = tmpSlice.get("segmentsMin");
      EXPECT_TRUE(tmpSlice2.isNumber() && 1 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsMax");
      EXPECT_TRUE(tmpSlice2.isNumber() && 10 == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesFloor");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(2) * (1 << 20)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("segmentsBytesMax");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (size_t(5) * (1 << 30)) == tmpSlice2.getNumber<size_t>());
      tmpSlice2 = tmpSlice.get("minScore");
      EXPECT_TRUE(tmpSlice2.isNumber() &&
                  (0. == tmpSlice2.getNumber<double>()));
    }
    tmpSlice = slice.get("writebufferActive");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                25 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferIdle");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                64 == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("writebufferSizeMax");
    EXPECT_TRUE(tmpSlice.isNumber<size_t>() &&
                32 * (size_t(1) << 20) == tmpSlice.getNumber<size_t>());
    tmpSlice = slice.get("primarySort");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(2, tmpSlice.length());
    {
      auto first = tmpSlice.at(0);
      ASSERT_TRUE(first.isObject());
      tmpSlice2 = first.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("my.Nested.field", tmpSlice2.copyString());
      tmpSlice2 = first.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_TRUE(tmpSlice2.getBoolean());
    }
    {
      auto second = tmpSlice.at(1);
      ASSERT_TRUE(second.isObject());
      tmpSlice2 = second.get("field");
      ASSERT_TRUE(tmpSlice2.isString());
      ASSERT_EQ("another.field", tmpSlice2.copyString());
      tmpSlice2 = second.get("asc");
      ASSERT_TRUE(tmpSlice2.isBoolean());
      ASSERT_FALSE(tmpSlice2.getBoolean());
    }
    tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("none"), tmpSlice.copyString());
    tmpSlice = slice.get("storedValues");
    EXPECT_TRUE(tmpSlice.isArray());
    EXPECT_EQ(0, tmpSlice.length());
  }
}

TEST_F(IResearchViewCoordinatorTest, test_overwrite_immutable_properties) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create view
  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", "
      "\"type\": \"arangosearch\", "
      "\"writebufferActive\": 25, "
      "\"writebufferIdle\": 12, "
      "\"writebufferSizeMax\": 44040192, "
      "\"locale\": \"C\", "
      "\"version\": 1, "
      "\"primarySortCompression\":\"none\","
      "\"primarySort\": [ "
      "{ \"field\": \"my.Nested.field\", \"direction\": \"asc\" }, "
      "{ \"field\": \"another.field\", \"asc\": false } "
      "]"
      "}");

  auto viewId = std::to_string(
      ci.uniqid() +
      1);  // +1 because LogicalView creation will generate a new ID

  EXPECT_TRUE(
      ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  auto view = ci.getView(vocbase->name(), viewId);
  EXPECT_NE(nullptr, view);
  EXPECT_NE(
      nullptr,
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_EQ("testView", view->name());
  EXPECT_FALSE(view->deleted());
  EXPECT_EQ(viewId, std::to_string(view->id().id()));
  EXPECT_EQ(arangodb::ViewType::kArangoSearch, view->type());
  EXPECT_EQ(arangodb::LogicalDataSource::Category::kView, view->category());
  EXPECT_EQ(vocbase, &view->vocbase());

  // check immutable properties after creation
  {
    arangodb::iresearch::IResearchViewMeta meta;
    std::string tmpString;
    VPackBuilder builder;

    builder.openObject();
    EXPECT_TRUE(
        view->properties(builder,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    builder.close();
    EXPECT_TRUE(true == meta.init(builder.slice(), tmpString));
    EXPECT_TRUE(1 == meta._version);
    EXPECT_TRUE(25 == meta._writebufferActive);
    EXPECT_TRUE(12 == meta._writebufferIdle);
    EXPECT_TRUE(42 * (size_t(1) << 20) == meta._writebufferSizeMax);
    EXPECT_TRUE(2 == meta._primarySort.size());
    {
      auto& field = meta._primarySort.field(0);
      EXPECT_TRUE(3 == field.size());
      EXPECT_TRUE("my" == field[0].name);
      EXPECT_TRUE(false == field[0].shouldExpand);
      EXPECT_TRUE("Nested" == field[1].name);
      EXPECT_TRUE(false == field[1].shouldExpand);
      EXPECT_TRUE("field" == field[2].name);
      EXPECT_TRUE(false == field[2].shouldExpand);
      EXPECT_TRUE(true == meta._primarySort.direction(0));
    }
    {
      auto& field = meta._primarySort.field(1);
      EXPECT_TRUE(2 == field.size());
      EXPECT_TRUE("another" == field[0].name);
      EXPECT_TRUE(false == field[0].shouldExpand);
      EXPECT_TRUE("field" == field[1].name);
      EXPECT_TRUE(false == field[1].shouldExpand);
      EXPECT_TRUE(false == meta._primarySort.direction(1));
    }
  }

  {
    auto newProperties = arangodb::velocypack::Parser::fromJson(
        "{"
        "\"writebufferActive\": 125, "
        "\"writebufferIdle\": 112, "
        "\"writebufferSizeMax\": 142, "
        "\"locale\": \"en\", "
        "\"version\": 1, "
        "\"primarySortCompression\":\"lz4\","
        "\"primarySort\": [ "
        "{ \"field\": \"field\", \"asc\": true } "
        "]"
        "}");

    decltype(view) fullyUpdatedView;

    // update properties
    EXPECT_TRUE(view->properties(newProperties->slice(), true, false).ok());
    EXPECT_EQ(planVersion,
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan version hasn't been changed as
                                      // nothing to update
    planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    fullyUpdatedView = ci.getView(vocbase->name(), viewId);
    EXPECT_EQ(fullyUpdatedView, view);  // same objects as nothing to update
  }

  {
    auto newProperties = arangodb::velocypack::Parser::fromJson(
        "{"
        "\"consolidationIntervalMsec\": 42, "
        "\"writebufferActive\": 125, "
        "\"writebufferIdle\": 112, "
        "\"writebufferSizeMax\": 142, "
        "\"locale\": \"en\", "
        "\"version\": 1, "
        "\"primarySortCompression\":\"lz4\","
        "\"primarySort\": [ "
        "{ \"field\": \"field\", \"asc\": true } "
        "]"
        "}");

    decltype(view) fullyUpdatedView;

    // update properties
    EXPECT_TRUE(view->properties(newProperties->slice(), true, false).ok());
    EXPECT_LT(planVersion, arangodb::tests::getCurrentPlanVersion(
                               server.server()));  // plan version changed
    planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    fullyUpdatedView = ci.getView(vocbase->name(), viewId);
    EXPECT_NE(fullyUpdatedView, view);  // different objects
    EXPECT_NE(nullptr, fullyUpdatedView);
    EXPECT_NE(nullptr, std::dynamic_pointer_cast<
                           arangodb::iresearch::IResearchViewCoordinator>(
                           fullyUpdatedView));
    EXPECT_EQ("testView", fullyUpdatedView->name());
    EXPECT_FALSE(fullyUpdatedView->deleted());
    EXPECT_EQ(viewId, std::to_string(fullyUpdatedView->id().id()));
    EXPECT_EQ(arangodb::ViewType::kArangoSearch, fullyUpdatedView->type());
    EXPECT_EQ(arangodb::LogicalDataSource::Category::kView,
              fullyUpdatedView->category());
    EXPECT_EQ(vocbase, &fullyUpdatedView->vocbase());

    // check immutable properties after update
    {
      arangodb::iresearch::IResearchViewMeta meta;
      std::string tmpString;
      VPackBuilder builder;

      builder.clear();
      builder.openObject();
      EXPECT_TRUE(
          fullyUpdatedView
              ->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();
      EXPECT_TRUE(true == meta.init(builder.slice(), tmpString));
      EXPECT_TRUE(1 == meta._version);
      EXPECT_TRUE(25 == meta._writebufferActive);
      EXPECT_TRUE(12 == meta._writebufferIdle);
      EXPECT_TRUE(42 * (size_t(1) << 20) == meta._writebufferSizeMax);
      EXPECT_EQ(irs::type<irs::compression::none>::id(),
                meta._primarySortCompression);
      EXPECT_TRUE(2 == meta._primarySort.size());
      {
        auto& field = meta._primarySort.field(0);
        EXPECT_TRUE(3 == field.size());
        EXPECT_TRUE("my" == field[0].name);
        EXPECT_TRUE(false == field[0].shouldExpand);
        EXPECT_TRUE("Nested" == field[1].name);
        EXPECT_TRUE(false == field[1].shouldExpand);
        EXPECT_TRUE("field" == field[2].name);
        EXPECT_TRUE(false == field[2].shouldExpand);
        EXPECT_TRUE(true == meta._primarySort.direction(0));
      }
      {
        auto& field = meta._primarySort.field(1);
        EXPECT_TRUE(2 == field.size());
        EXPECT_TRUE("another" == field[0].name);
        EXPECT_TRUE(false == field[0].shouldExpand);
        EXPECT_TRUE("field" == field[1].name);
        EXPECT_TRUE(false == field[1].shouldExpand);
        EXPECT_TRUE(false == meta._primarySort.direction(1));
      }
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_links_partial_remove) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  {
    auto const collectionId = "1";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection1);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;
  {
    auto const collectionId = "2";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection2\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection2 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection2);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection3;
  {
    auto const collectionId = "3";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"3\", \"planId\": \"3\",  \"name\": \"testCollection3\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection3 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection3);
  }

  auto const currentCollection1Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection1->id().id());
  auto const currentCollection2Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection2->id().id());
  auto const currentCollection3Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection3->id().id());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  // create view
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(
      arangodb::LogicalView::create(view, *vocbase, viewJson->slice(), false)
          .ok());
  ASSERT_TRUE(view);
  auto const viewId = std::to_string(view->planId().id());
  EXPECT_TRUE("42" == viewId);

  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  // explicitly specify id for the sake of tests
  auto linksJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"testCollection1\" : { \"id\": \"1\", \"includeAllFields\" : true "
      "}, "
      "  \"2\" : { \"id\": \"2\", \"trackListPositions\" : true }, "
      "  \"testCollection3\" : { \"id\": \"3\" } "
      "} }");
  EXPECT_TRUE(
      view->properties(linksJson->slice(), true, true).ok());  // add links
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  auto oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection2->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(3 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection2
    {
      auto const value = linksSlice.get(logicalCollection2->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._trackListPositions = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection2
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._trackListPositions = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  EXPECT_TRUE(view->properties(linksJson->slice(), true, true)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  // remove testCollection2 link
  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }

  auto const updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"2\" : null "
      "} }");
  EXPECT_TRUE(view->properties(updateJson->slice(), true, true).ok());
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(2 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // drop view
  // simulate heartbeat thread (drop index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  EXPECT_TRUE(view->drop().ok());
  EXPECT_TRUE(planVersion <
              arangodb::tests::getCurrentPlanVersion(server.server()));

  // there is no more view
  ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));

  // there are no more links
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_links_partial_add) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  {
    auto const collectionId = "1";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection1);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;
  {
    auto const collectionId = "2";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection2\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection2 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection2);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection3;
  {
    auto const collectionId = "3";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"3\", \"planId\": \"3\",  \"name\": \"testCollection3\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection3 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection3);
  }

  auto const currentCollection1Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection1->id().id());
  auto const currentCollection2Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection2->id().id());
  auto const currentCollection3Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection3->id().id());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  // create view
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(
      (arangodb::LogicalView::create(view, *vocbase, viewJson->slice(), false)
           .ok()));
  ASSERT_TRUE(view);
  auto const viewId = std::to_string(view->planId().id());
  EXPECT_TRUE("42" == viewId);

  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  // explicitly specify id for the sake of tests
  auto linksJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"testCollection1\" : { \"id\": \"1\", \"includeAllFields\" : true "
      "}, "
      "  \"testCollection3\" : { \"id\": \"3\" } "
      "} }");
  EXPECT_TRUE(
      view->properties(linksJson->slice(), true, true).ok());  // add links
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  auto oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(2 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  EXPECT_TRUE(view->properties(linksJson->slice(), true, true)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  // remove testCollection2 link
  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }

  auto const updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"2\" : { \"id\": \"2\", \"trackListPositions\" : true } "
      "} }");
  EXPECT_TRUE(view->properties(updateJson->slice(), true, true).ok());
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection2->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(3 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection2
    {
      auto const value = linksSlice.get(logicalCollection2->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._trackListPositions = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection2
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._trackListPositions = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // partial update - empty delta
  {
    auto const updateJson = arangodb::velocypack::Parser::fromJson("{ }");
    EXPECT_TRUE(
        view->properties(updateJson->slice(), true, true)
            .ok());  // empty properties -> should not affect plan version
    EXPECT_TRUE(planVersion ==
                arangodb::tests::getCurrentPlanVersion(
                    server.server()));  // plan did't change version
  }

  // drop view
  // simulate heartbeat thread (drop index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  EXPECT_TRUE(view->drop().ok());
  EXPECT_TRUE(planVersion <
              arangodb::tests::getCurrentPlanVersion(server.server()));

  // there is no more view
  ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));

  // there are no more links
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  // add link (collection not authorized)
  {
    auto const collectionId = "1";
    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection1));
    arangodb::LogicalView::ptr logicalView;
    ASSERT_TRUE(arangodb::LogicalView::create(logicalView, *vocbase,
                                              viewJson->slice(), false)
                    .ok());
    ASSERT_TRUE((false == !logicalView));

    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    arangodb::auth::UserMap userMap;    // empty map, no user -> no permissions
    userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                        // configuration from system database

    irs::Finally resetUserManager = [userManager]() noexcept {
      userManager->removeAllUsers();
    };

    EXPECT_TRUE((
        TRI_ERROR_FORBIDDEN ==
        logicalView->properties(linksJson->slice(), true, true).errorNumber()));
    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection1));
    logicalView =
        ci.getView(vocbase->name(), viewId);  // get new version of the view
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_links_replace) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  {
    auto const collectionId = "1";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection1);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;
  {
    auto const collectionId = "2";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection2\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection2 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection2);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection3;
  {
    auto const collectionId = "3";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"3\", \"planId\": \"3\",  \"name\": \"testCollection3\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection3 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection3);
  }

  auto const currentCollection1Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection1->id().id());
  auto const currentCollection2Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection2->id().id());
  auto const currentCollection3Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection3->id().id());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  // create view
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(
      (arangodb::LogicalView::create(view, *vocbase, viewJson->slice(), false)
           .ok()));
  ASSERT_TRUE(view);
  auto const viewId = std::to_string(view->planId().id());
  EXPECT_TRUE("42" == viewId);

  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  // explicitly specify id for the sake of tests
  auto linksJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"testCollection1\" : { \"id\": \"1\", \"includeAllFields\" : true "
      "}, "
      "  \"testCollection3\" : { \"id\": \"3\" } "
      "} }");
  EXPECT_TRUE(
      view->properties(linksJson->slice(), true, false).ok());  // add link
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  auto oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(2 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  EXPECT_TRUE(view->properties(linksJson->slice(), true, false)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  EXPECT_TRUE(view->properties(linksJson->slice(), true, true)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  // replace links with testCollection2 link
  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"2\" : { \"id\": \"2\", \"trackListPositions\" : true } "
      "} }");
  EXPECT_TRUE(view->properties(updateJson->slice(), true, false).ok());
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection2->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(1 == it.size());

    // testCollection2
    {
      auto const value = linksSlice.get(logicalCollection2->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._trackListPositions = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection2
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._trackListPositions = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // replace links with testCollection1 link
  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\" : \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }

  updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
      "  \"testCollection1\" : { \"id\": \"1\", \"includeAllFields\" : true "
      "}, "
      "  \"2\" : null "
      "} }");
  EXPECT_TRUE(view->properties(updateJson->slice(), true, false).ok());
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(1 == it.size());

    // testCollection2
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // drop view
  // simulate heartbeat thread (drop index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }

  EXPECT_TRUE(view->drop().ok());
  EXPECT_TRUE(planVersion <
              arangodb::tests::getCurrentPlanVersion(server.server()));

  // there is no more view
  ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));

  // there are no more links
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_links_clear) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collections
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  {
    auto const collectionId = "1";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection1);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;
  {
    auto const collectionId = "2";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection2\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection2 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection2);
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection3;
  {
    auto const collectionId = "3";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"3\", \"planId\": \"3\",  \"name\": \"testCollection3\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection3 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE(nullptr != logicalCollection3);
  }

  auto const currentCollection1Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection1->id().id());
  auto const currentCollection2Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection2->id().id());
  auto const currentCollection3Path =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection3->id().id());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  // create view
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"id\": \"42\", \"type\": \"arangosearch\" "
      "}");
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(
      arangodb::LogicalView::create(view, *vocbase, viewJson->slice(), false)
          .ok());
  ASSERT_TRUE(view);
  auto const viewId = std::to_string(view->planId().id());
  EXPECT_TRUE("42" == viewId);

  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  // explicitly specify id for the sake of tests
  auto linksJson = arangodb::velocypack::Parser::fromJson(
      "{ \"locale\": \"en\", \"links\": {"
      "  \"testCollection1\" : { \"id\": \"1\", \"includeAllFields\" : true "
      "}, "
      "  \"2\" : { \"id\": \"2\", \"trackListPositions\" : true }, "
      "  \"testCollection3\" : { \"id\": \"3\" } "
      "} }");
  EXPECT_TRUE(
      view->properties(linksJson->slice(), true, false).ok());  // add link
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  auto oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    std::set<arangodb::DataSourceId> expectedLinks{logicalCollection1->id(),
                                                   logicalCollection2->id(),
                                                   logicalCollection3->id()};
    EXPECT_TRUE(view->visitCollections(
        [&expectedLinks](arangodb::DataSourceId cid,
                         arangodb::LogicalView::Indexes*) mutable {
          return 1 == expectedLinks.erase(cid);
        }));
    EXPECT_TRUE(expectedLinks.empty());
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
    auto const linksSlice =
        properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE(linksSlice.isObject());

    VPackObjectIterator it(linksSlice);
    EXPECT_TRUE(it.valid());
    EXPECT_TRUE(3 == it.size());

    // testCollection1
    {
      auto const value = linksSlice.get(logicalCollection1->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection2
    {
      auto const value = linksSlice.get(logicalCollection2->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._trackListPositions = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    // testCollection3
    {
      auto const value = linksSlice.get(logicalCollection3->name());
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._includeAllFields = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection2
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    expectedMeta._trackListPositions = true;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  // test index in testCollection3
  {
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(link);

    auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
    ASSERT_TRUE((false == !index));
    EXPECT_TRUE((true == index->canBeDropped()));
    EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
    EXPECT_TRUE((index->fieldNames().empty()));
    EXPECT_TRUE((index->fields().empty()));
    EXPECT_TRUE((false == index->hasExpansion()));
    EXPECT_TRUE((false == index->hasSelectivityEstimate()));
    EXPECT_TRUE((false == index->implicitlyUnique()));
    EXPECT_TRUE((false == index->isSorted()));
    EXPECT_EQ(0, index->memory());
    EXPECT_TRUE((true == index->sparse()));
    EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                 index->type()));
    EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                 index->typeName()));
    EXPECT_TRUE((false == index->unique()));

    arangodb::iresearch::IResearchLinkMeta expectedMeta;
    arangodb::iresearch::IResearchLinkMeta actualMeta;
    auto builder = index->toVelocyPack(
        arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

    std::string error;
    EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(expectedMeta == actualMeta);
    auto const slice = builder->slice();
    EXPECT_TRUE(slice.hasKey("view"));
    EXPECT_TRUE(slice.get("view").isString());
    EXPECT_TRUE(view->id().id() == 42);
    EXPECT_TRUE(view->guid() == slice.get("view").copyString());
    EXPECT_TRUE(slice.hasKey("figures"));
    auto figuresSlice = slice.get("figures");
    EXPECT_TRUE(figuresSlice.isObject());
    EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
    EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
    EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
    EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
    EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
    EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
    EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
    EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
    EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
    EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
  }

  EXPECT_TRUE(view->properties(linksJson->slice(), true, false)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  EXPECT_TRUE(view->properties(linksJson->slice(), true, true)
                  .ok());  // same properties -> should not affect plan version
  EXPECT_TRUE(planVersion ==
              arangodb::tests::getCurrentPlanVersion(
                  server.server()));  // plan did't change version

  // remove all links
  // simulate heartbeat thread (create index in current)
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection1Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection2Path, value->slice(), 0.0)
                    .successful());
  }
  {
    auto const value = arangodb::velocypack::Parser::fromJson(
        "{ \"s12345\": { \"indexes\" : [ ] } }");
    EXPECT_TRUE(arangodb::AgencyComm(server.server())
                    .setValue(currentCollection3Path, value->slice(), 0.0)
                    .successful());
  }

  auto const updateJson =
      arangodb::velocypack::Parser::fromJson("{ \"links\": {} }");
  EXPECT_TRUE(view->properties(updateJson->slice(), true, false).ok());
  EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                server.server()));  // plan version changed
  planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
  oldView = view;
  view = ci.getView(vocbase->name(), viewId);  // get new version of the view
  EXPECT_TRUE(view != oldView);                // different objects
  ASSERT_TRUE(nullptr != view);
  ASSERT_TRUE(
      nullptr !=
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchViewCoordinator>(
          view));
  EXPECT_TRUE("testView" == view->name());
  EXPECT_TRUE(false == view->deleted());
  EXPECT_TRUE(42 == view->id().id());
  EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
  EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView == view->category());
  EXPECT_TRUE(vocbase == &view->vocbase());

  // visit collections
  {
    EXPECT_TRUE(view->visitCollections(
        [](arangodb::DataSourceId cid,
           arangodb::LogicalView::Indexes*) mutable { return false; }));
  }

  // check properties
  {
    VPackBuilder info;
    info.openObject();
    EXPECT_TRUE(
        view->properties(info,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    info.close();

    auto const properties = info.slice();
    EXPECT_TRUE(
        (properties.hasKey(arangodb::iresearch::StaticStrings::LinksField)));
    auto links = properties.get(arangodb::iresearch::StaticStrings::LinksField);
    EXPECT_TRUE((links.isObject()));
    EXPECT_TRUE((0 == links.length()));
  }

  // test index in testCollection1
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection1->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  // test index in testCollection2
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection2->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  // test index in testCollection3
  {
    // get new version from plan
    auto updatedCollection = ci.getCollection(
        vocbase->name(), std::to_string(logicalCollection3->id().id()));
    ASSERT_TRUE(updatedCollection);
    auto link = arangodb::iresearch::IResearchLinkHelper::find(
        *updatedCollection, *view);
    EXPECT_TRUE(!link);
  }

  // drop view
  EXPECT_TRUE(view->drop().ok());
  EXPECT_TRUE(planVersion <
              arangodb::tests::getCurrentPlanVersion(server.server()));

  // there is no more view
  ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));
}

TEST_F(IResearchViewCoordinatorTest, test_drop_link) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create collection
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  {
    auto const collectionId = "1";

    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));

    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((nullptr != logicalCollection));
  }

  auto const currentCollectionPath =
      "/Current/Collections/" + vocbase->name() + "/" +
      std::to_string(logicalCollection->id().id());

  // get current plan version
  auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

  // update link
  {
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    arangodb::LogicalView::ptr logicalView;
    ASSERT_TRUE((arangodb::LogicalView::create(logicalView, *vocbase,
                                               viewJson->slice(), false)
                     .ok()));
    auto view = std::dynamic_pointer_cast<
        arangodb::iresearch::IResearchViewCoordinator>(logicalView);
    ASSERT_TRUE(view);
    auto const viewId = std::to_string(view->planId().id());
    EXPECT_TRUE("42" == viewId);

    // simulate heartbeat thread (create index in current)
    {
      auto const value = arangodb::velocypack::Parser::fromJson(
          "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(currentCollectionPath, value->slice(), 0.0)
                      .successful());
    }

    auto linksJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\" : { \"includeAllFields\" : true } "
        "} }");
    EXPECT_TRUE(
        view->properties(linksJson->slice(), true, true).ok());  // add link
    EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                  server.server()));  // plan version changed
    planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    auto oldView = view;
    view = std::dynamic_pointer_cast<
        arangodb::iresearch::IResearchViewCoordinator>(
        ci.getView(vocbase->name(), viewId));  // get new version of the view
    EXPECT_TRUE(view != oldView);              // different objects
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(42 == view->id().id());
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // visit collections
    {
      std::set<arangodb::DataSourceId> expectedLinks{logicalCollection->id()};
      EXPECT_TRUE(view->visitCollections(
          [&expectedLinks](arangodb::DataSourceId cid,
                           arangodb::LogicalView::Indexes*) mutable {
            return 1 == expectedLinks.erase(cid);
          }));
      EXPECT_TRUE(expectedLinks.empty());
    }

    // check properties
    {
      VPackBuilder info;
      info.openObject();
      EXPECT_TRUE(
          view->properties(
                  info, arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      info.close();

      auto const properties = info.slice();
      EXPECT_TRUE(
          properties.hasKey(arangodb::iresearch::StaticStrings::LinksField));
      auto const linksSlice =
          properties.get(arangodb::iresearch::StaticStrings::LinksField);
      EXPECT_TRUE(linksSlice.isObject());

      VPackObjectIterator it(linksSlice);
      EXPECT_TRUE(it.valid());
      EXPECT_TRUE(1 == it.size());
      auto const& valuePair = *it;
      auto const key = valuePair.key;
      EXPECT_TRUE(key.isString());
      EXPECT_TRUE("testCollection" == key.copyString());
      auto const value = valuePair.value;
      EXPECT_TRUE(value.isObject());

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), value, error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
    }

    arangodb::IndexId linkId;

    // check indexes
    {
      // get new version from plan
      auto updatedCollection = ci.getCollection(
          vocbase->name(), std::to_string(logicalCollection->id().id()));
      ASSERT_TRUE(updatedCollection);
      auto link = arangodb::iresearch::IResearchLinkHelper::find(
          *updatedCollection, *view);
      ASSERT_TRUE((link));
      linkId = link->index().id();

      auto index = std::dynamic_pointer_cast<arangodb::Index>(link);
      ASSERT_TRUE((false == !index));
      EXPECT_TRUE((true == index->canBeDropped()));
      EXPECT_TRUE((updatedCollection.get() == &(index->collection())));
      EXPECT_TRUE((index->fieldNames().empty()));
      EXPECT_TRUE((index->fields().empty()));
      EXPECT_TRUE((false == index->hasExpansion()));
      EXPECT_TRUE((false == index->hasSelectivityEstimate()));
      EXPECT_TRUE((false == index->implicitlyUnique()));
      EXPECT_TRUE((false == index->isSorted()));
      EXPECT_EQ(0, index->memory());
      EXPECT_TRUE((true == index->sparse()));
      EXPECT_TRUE((arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK ==
                   index->type()));
      EXPECT_TRUE((arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                   index->typeName()));
      EXPECT_TRUE((false == index->unique()));

      arangodb::iresearch::IResearchLinkMeta expectedMeta;
      expectedMeta._includeAllFields = true;
      arangodb::iresearch::IResearchLinkMeta actualMeta;
      auto builder = index->toVelocyPack(
          arangodb::Index::makeFlags(arangodb::Index::Serialize::Figures));

      std::string error;
      EXPECT_TRUE(actualMeta.init(server.server(), builder->slice(), error));
      EXPECT_TRUE(error.empty());
      EXPECT_TRUE(expectedMeta == actualMeta);
      auto const slice = builder->slice();
      EXPECT_TRUE(slice.hasKey("view"));
      EXPECT_TRUE(slice.get("view").isString());
      EXPECT_TRUE(view->id().id() == 42);
      EXPECT_TRUE(view->guid() == slice.get("view").copyString());
      EXPECT_TRUE(slice.hasKey("figures"));
      auto figuresSlice = slice.get("figures");
      EXPECT_TRUE(figuresSlice.isObject());
      EXPECT_TRUE(figuresSlice.hasKey("indexSize"));
      EXPECT_TRUE(figuresSlice.get("indexSize").isNumber());
      EXPECT_EQ(0, figuresSlice.get("indexSize").getNumber<size_t>());
      EXPECT_TRUE(figuresSlice.hasKey("numFiles"));
      EXPECT_TRUE(figuresSlice.get("numFiles").isNumber());
      EXPECT_EQ(0, figuresSlice.get("numFiles").getNumber<size_t>());
      EXPECT_TRUE(figuresSlice.hasKey("numDocs"));
      EXPECT_TRUE(figuresSlice.get("numDocs").isNumber());
      EXPECT_EQ(0, figuresSlice.get("numDocs").getNumber<size_t>());
      EXPECT_TRUE(figuresSlice.hasKey("numLiveDocs"));
      EXPECT_TRUE(figuresSlice.get("numLiveDocs").isNumber());
      EXPECT_EQ(0, figuresSlice.get("numLiveDocs").getNumber<size_t>());
      EXPECT_TRUE(figuresSlice.hasKey("numPrimaryDocs"));
      EXPECT_TRUE(figuresSlice.get("numPrimaryDocs").isNumber());
      EXPECT_EQ(0, figuresSlice.get("numPrimaryDocs").getNumber<size_t>());
      EXPECT_TRUE(figuresSlice.hasKey("numSegments"));
      EXPECT_TRUE(figuresSlice.get("numSegments").isNumber());
      EXPECT_EQ(0, figuresSlice.get("numSegments").getNumber<size_t>());
    }

    EXPECT_TRUE(
        view->properties(linksJson->slice(), true, true)
            .ok());  // same properties -> should not affect plan version
    EXPECT_TRUE(planVersion ==
                arangodb::tests::getCurrentPlanVersion(
                    server.server()));  // plan did't change version

    // simulate heartbeat thread (drop index from current)
    {
      auto const value = arangodb::velocypack::Parser::fromJson(
          "{ \"s12345\": { \"indexes\" : [ ] } }");
      EXPECT_TRUE(arangodb::AgencyComm(server.server())
                      .setValue(currentCollectionPath, value->slice(), 0.0)
                      .successful());
    }

    // drop link
    EXPECT_TRUE((arangodb::methods::Indexes::drop(
                     *logicalCollection, arangodb::velocypack::Parser::fromJson(
                                             std::to_string(linkId.id()))
                                             ->slice())
                     .waitAndGet()
                     .ok()));
    EXPECT_TRUE(planVersion < arangodb::tests::getCurrentPlanVersion(
                                  server.server()));  // plan version changed
    planVersion = arangodb::tests::getCurrentPlanVersion(server.server());
    oldView = view;
    view = std::dynamic_pointer_cast<
        arangodb::iresearch::IResearchViewCoordinator>(
        ci.getView(vocbase->name(), viewId));  // get new version of the view
    EXPECT_TRUE(view != oldView);              // different objects
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(42 == view->id().id());
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // visit collections
    EXPECT_TRUE(view->visitCollections(
        [](arangodb::DataSourceId, arangodb::LogicalView::Indexes*) {
          return false;
        }));

    // check properties
    {
      VPackBuilder info;
      info.openObject();
      EXPECT_TRUE(
          view->properties(
                  info, arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      info.close();

      auto const properties = info.slice();
      EXPECT_TRUE(
          (properties.hasKey(arangodb::iresearch::StaticStrings::LinksField)));
      auto links =
          properties.get(arangodb::iresearch::StaticStrings::LinksField);
      EXPECT_TRUE((links.isObject()));
      EXPECT_TRUE((0 == links.length()));
    }

    // check indexes
    {
      // get new version from plan
      auto updatedCollection = ci.getCollection(
          vocbase->name(), std::to_string(logicalCollection->id().id()));
      ASSERT_TRUE(updatedCollection);
      auto link = arangodb::iresearch::IResearchLinkHelper::find(
          *updatedCollection, *view);
      EXPECT_TRUE(!link);
    }

    // drop view
    EXPECT_TRUE(view->drop().ok());
    EXPECT_TRUE(planVersion <
                arangodb::tests::getCurrentPlanVersion(server.server()));

    // there is no more view
    ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));
  }

  // add link (collection not authorized)
  {
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\" : { \"includeAllFields\" : true } "
        "} }");
    auto const collectionId = "1";
    auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection1));
    arangodb::LogicalView::ptr logicalView;
    ASSERT_TRUE(arangodb::LogicalView::create(logicalView, *vocbase,
                                              viewCreateJson->slice(), true)
                    .ok());
    ASSERT_TRUE((false == !logicalView));
    auto const viewId = std::to_string(logicalView->planId().id());

    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    arangodb::auth::UserMap userMap;    // empty map, no user -> no permissions
    userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                        // configuration from system database

    irs::Finally resetUserManager = [userManager]() noexcept {
      userManager->removeAllUsers();
    };

    EXPECT_EQ(TRI_ERROR_FORBIDDEN,
              logicalView->properties(viewUpdateJson->slice(), true, false)
                  .errorNumber());
    logicalCollection1 = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection1));
    logicalView =
        ci.getView(vocbase->name(), viewId);  // get new version of the view
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_overwrite) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // modify meta params with links to missing collections
  {
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }");
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    EXPECT_EQ(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
              logicalView->properties(viewUpdateJson->slice(), true, false)
                  .errorNumber());
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::velocypack::Builder builder;
    builder.openObject();
    EXPECT_TRUE(logicalView
                    ->properties(
                        builder,
                        arangodb::LogicalDataSource::Serialization::Persistence)
                    .ok());  // 'forPersistence' to avoid auth check
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;
    EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
  }

  // modify meta params with links with invalid definition
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": 42 } }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    EXPECT_EQ(TRI_ERROR_BAD_PARAMETER,
              logicalView->properties(viewUpdateJson->slice(), true, false)
                  .errorNumber());
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::velocypack::Builder builder;
    builder.openObject();
    EXPECT_TRUE(logicalView
                    ->properties(
                        builder,
                        arangodb::LogicalDataSource::Serialization::Persistence)
                    .ok());  // 'forPersistence' to avoid auth check
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;
    EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
  }

  // modify meta params with links (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"cleanupIntervalStep\": 62, \"links\": { \"testCollection\": {} "
        "} }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::NONE);   // for missing collections
                                          // User::collectionAuthLevel(...)
                                          // returns database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      arangodb::iresearch::IResearchViewMeta expectedMeta;

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, false)
                    .errorNumber());
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));

      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          logicalView
              ->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());  // 'forPersistence' to avoid auth check
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      arangodb::iresearch::IResearchViewMeta expectedMeta;
      expectedMeta._cleanupIntervalStep = 62;

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, false).ok());
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));

      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          logicalView
              ->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());  // 'forPersistence' to avoid auth check
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }
  }

  // add link (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();
    arangodb::auth::UserMap userMap;    // empty map, no user -> no permissions
    userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                        // configuration from system database
    irs::Finally resetUserManager = [userManager]() noexcept {
      userManager->removeAllUsers();
    };

    EXPECT_EQ(TRI_ERROR_FORBIDDEN,
              logicalView->properties(viewUpdateJson->slice(), true, false)
                  .errorNumber());
    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));
  }

  // drop link (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null } }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));

      // simulate heartbeat thread (update index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::NONE);   // for missing collections
                                          // User::collectionAuthLevel(...)
                                          // returns database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, false)
                    .errorNumber());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, false).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }

  // add authorised link (existing collection not authorized)
  {
    auto collection0Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection0\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto collection1Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection0\": {}, \"testCollection1\": {} } "
        "}");
    auto collectionId0 = std::to_string(1);
    auto collectionId1 = std::to_string(2);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId0, 0, 1, 1, false,
                       collection0Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
    ASSERT_TRUE((false == !logicalCollection0));
    auto dropLogicalCollection0 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId0](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId0, 0);
        });
    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId1, 0, 1, 1, false,
                       collection1Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
    ASSERT_TRUE((false == !logicalCollection1));
    auto dropLogicalCollection1 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId1](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId1, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection0->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path0 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection0->id().id());
        auto const path1 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection1->id().id());
        auto const value0 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
        auto const value1 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"4\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path0, value0->slice(), 0.0)
                        .successful());
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path1, value1->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection0\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::NONE);  // for missing collections
                                         // User::collectionAuthLevel(...)
                                         // returns database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, false)
                    .errorNumber());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, false).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }

  // drop authorised link (existing collection not authorized)
  {
    auto collection0Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection0\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto collection1Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection0\": {} } }");
    auto collectionId0 = std::to_string(1);
    auto collectionId1 = std::to_string(2);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId0, 0, 1, 1, false,
                       collection0Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
    ASSERT_TRUE((false == !logicalCollection0));
    auto dropLogicalCollection0 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId0](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId0, 0);
        });
    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId1, 0, 1, 1, false,
                       collection1Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
    ASSERT_TRUE((false == !logicalCollection1));
    auto dropLogicalCollection1 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId1](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId1, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection0->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path0 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection0->id().id());
        auto const path1 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection1->id().id());
        auto const value0 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"5\" } ] } }");
        auto const value1 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"6\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path0, value0->slice(), 0.0)
                        .successful());
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path1, value1->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection0\": {}, \"testCollection1\": {} } "
          "}");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));

      // simulate heartbeat thread (remove index from current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection1->id().id()) +
                          "/s12345/indexes";
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .removeValues(path, false)
                        .successful());
      }
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::NONE);  // for missing collections
                                         // User::collectionAuthLevel(...)
                                         // returns database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, false)
                    .errorNumber());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, false).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, test_update_partial) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // modify meta params with links to missing collections
  {
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"cleanupIntervalStep\": 62, \"links\": { \"testCollection\": {} "
        "} }");
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    EXPECT_EQ(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
              logicalView->properties(viewUpdateJson->slice(), true, true)
                  .errorNumber());
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::velocypack::Builder builder;
    builder.openObject();
    EXPECT_TRUE(logicalView
                    ->properties(
                        builder,
                        arangodb::LogicalDataSource::Serialization::Persistence)
                    .ok());  // 'forPersistence' to avoid auth check
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;
    EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
  }

  // modify meta params with links with invalid definition
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"type\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"cleanupIntervalStep\": 62, \"links\": { \"testCollection\": 42 "
        "} }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    EXPECT_EQ(TRI_ERROR_BAD_PARAMETER,
              logicalView->properties(viewUpdateJson->slice(), true, true)
                  .errorNumber());
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    arangodb::velocypack::Builder builder;
    builder.openObject();
    EXPECT_TRUE(logicalView
                    ->properties(
                        builder,
                        arangodb::LogicalDataSource::Serialization::Persistence)
                    .ok());  // 'forPersistence' to avoid auth check
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;
    EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
  }

  // modify meta params with links (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"cleanupIntervalStep\": 62 }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"1\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::NONE);   // for missing collections
                                          // User::collectionAuthLevel(...)
                                          // returns database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      arangodb::iresearch::IResearchViewMeta expectedMeta;

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, true)
                    .errorNumber());
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));

      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          logicalView
              ->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());  // 'forPersistence' to avoid auth check
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      arangodb::iresearch::IResearchViewMeta expectedMeta;
      expectedMeta._cleanupIntervalStep = 62;

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, true).ok());
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));

      arangodb::velocypack::Builder builder;
      builder.openObject();
      EXPECT_TRUE(
          logicalView
              ->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());  // 'forPersistence' to avoid auth check
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;
      EXPECT_TRUE((meta.init(slice, error) && expectedMeta == meta));
    }
  }

  // add link (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();
    arangodb::auth::UserMap userMap;    // empty map, no user -> no permissions
    userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                        // configuration from system database
    irs::Finally resetUserManager = [userManager]() noexcept {
      userManager->removeAllUsers();
    };

    EXPECT_EQ(TRI_ERROR_FORBIDDEN,
              logicalView->properties(viewUpdateJson->slice(), true, true)
                  .errorNumber());
    logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));
  }

  // drop link (collection not authorized)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null } }");
    auto collectionId = std::to_string(1);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId, 0, 1, 1, false,
                       collectionJson->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection = ci.getCollection(vocbase->name(), collectionId);
    ASSERT_TRUE((false == !logicalCollection));
    auto dropLogicalCollection = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"2\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));

      // simulate heartbeat thread (remove index from current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection->id().id()) +
                          "/s12345/indexes";
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .removeValues(path, false)
                        .successful());
      }
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::NONE);   // for missing collections
                                          // User::collectionAuthLevel(...)
                                          // returns database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, true)
                    .errorNumber());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (false == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, true).ok());
      logicalCollection = ci.getCollection(vocbase->name(), collectionId);
      ASSERT_TRUE((false == !logicalCollection));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE(
          (true == logicalCollection->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }

  // add authorised link (existing collection not authorized)
  {
    auto collection0Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection0\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto collection1Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection1\": {} } }");
    auto collectionId0 = std::to_string(1);
    auto collectionId1 = std::to_string(2);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId0, 0, 1, 1, false,
                       collection0Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
    ASSERT_TRUE((false == !logicalCollection0));
    auto dropLogicalCollection0 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId0](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId0, 0);
        });
    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId1, 0, 1, 1, false,
                       collection1Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
    ASSERT_TRUE((false == !logicalCollection1));
    auto dropLogicalCollection1 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId1](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId1, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection0->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection0->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"3\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path, value->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection0\": {} } }");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));

      // simulate heartbeat thread (update index in current)
      {
        auto const path0 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection0->id().id());
        auto const path1 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection1->id().id());
        auto const value = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"4\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .removeValues(path0, false)
                        .successful());
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path1, value->slice(), 0.0)
                        .successful());
      }
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::NONE);  // for missing collections
                                         // User::collectionAuthLevel(...)
                                         // returns database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, true)
                    .errorNumber());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }

  // drop authorised link (existing collection not authorized)
  {
    auto collection0Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"1\", \"planId\": \"1\",  \"name\": \"testCollection0\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto collection1Json = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"2\", \"planId\": \"2\",  \"name\": \"testCollection1\", "
        "\"replicationFactor\": 1, \"shards\":{} }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"id\": \"42\", \"type\": "
        "\"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection1\": null } }");
    auto collectionId0 = std::to_string(1);
    auto collectionId1 = std::to_string(2);
    auto viewId = std::to_string(42);

    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId0, 0, 1, 1, false,
                       collection0Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
    ASSERT_TRUE((false == !logicalCollection0));
    auto dropLogicalCollection0 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId0](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId0, 0);
        });
    EXPECT_TRUE((ci.createCollectionCoordinator(
                       vocbase->name(), collectionId1, 0, 1, 1, false,
                       collection1Json->slice(), 0.0, false, nullptr,
                       arangodb::replication::Version::ONE)
                     .ok()));
    auto logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
    ASSERT_TRUE((false == !logicalCollection1));
    auto dropLogicalCollection1 = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &collectionId1](arangodb::ClusterInfo* ci) -> void {
          ci->dropCollectionCoordinator(vocbase->name(), collectionId1, 0);
        });
    EXPECT_TRUE((ci.createViewCoordinator(vocbase->name(), viewId,
                                          viewCreateJson->slice())
                     .ok()));
    auto logicalView = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE((false == !logicalView));
    auto dropLogicalView = std::shared_ptr<arangodb::ClusterInfo>(
        &ci, [vocbase, &viewId](arangodb::ClusterInfo* ci) -> void {
          ci->dropViewCoordinator(vocbase->name(), viewId);
        });

    EXPECT_TRUE(
        (true == logicalCollection0->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
    EXPECT_TRUE(
        (true == logicalView->visitCollections(
                     [](arangodb::DataSourceId,
                        arangodb::LogicalView::Indexes*) { return false; })));

    // initial link creation
    {
      // simulate heartbeat thread (create index in current)
      {
        auto const path0 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection0->id().id());
        auto const path1 = "/Current/Collections/" + vocbase->name() + "/" +
                           std::to_string(logicalCollection1->id().id());
        auto const value0 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"5\" } ] } }");
        auto const value1 = arangodb::velocypack::Parser::fromJson(
            "{ \"s12345\": { \"indexes\" : [ { \"id\": \"6\" } ] } }");
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path0, value0->slice(), 0.0)
                        .successful());
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .setValue(path1, value1->slice(), 0.0)
                        .successful());
      }

      auto updateJson = arangodb::velocypack::Parser::fromJson(
          "{ \"links\": { \"testCollection0\": {}, \"testCollection1\": {} } "
          "}");
      EXPECT_TRUE(
          logicalView->properties(updateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));

      // simulate heartbeat thread (remove index from current)
      {
        auto const path = "/Current/Collections/" + vocbase->name() + "/" +
                          std::to_string(logicalCollection1->id().id()) +
                          "/s12345/indexes";
        EXPECT_TRUE(arangodb::AgencyComm(server.server())
                        .removeValues(path, false)
                        .successful());
      }
    }

    struct ExecContext : public arangodb::ExecContext {
      ExecContext()
          : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                  arangodb::ExecContext::Type::Default, "", "",
                                  arangodb::auth::Level::NONE,
                                  arangodb::auth::Level::NONE, false) {}
    };
    auto execContext = std::make_shared<ExecContext>();
    arangodb::ExecContextScope execContextScope(execContext);
    auto* authFeature = arangodb::AuthenticationFeature::instance();
    auto* userManager = authFeature->userManager();

    auto resetUserManager = std::shared_ptr<arangodb::auth::UserManager>(
        userManager, [](arangodb::auth::UserManager* ptr) -> void {
          ptr->removeAllUsers();
        });

    // subsequent update (overwrite) not authorised (NONE collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::NONE);  // for missing collections
                                         // User::collectionAuthLevel(...)
                                         // returns database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_EQ(TRI_ERROR_FORBIDDEN,
                logicalView->properties(viewUpdateJson->slice(), true, true)
                    .errorNumber());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }

    // subsequent update (overwrite) authorised (RO collection)
    {
      arangodb::auth::UserMap userMap;
      auto& user = userMap.emplace("", arangodb::auth::User::newUser("", ""))
                       .first->second;
      user.grantCollection(
          vocbase->name(), "testCollection0",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      user.grantCollection(
          vocbase->name(), "testCollection1",
          arangodb::auth::Level::RO);  // for missing collections
                                       // User::collectionAuthLevel(...) returns
                                       // database auth::Level
      userManager->setAuthInfo(userMap);  // set user map to avoid loading
                                          // configuration from system database

      EXPECT_TRUE(
          logicalView->properties(viewUpdateJson->slice(), true, true).ok());
      logicalCollection0 = ci.getCollection(vocbase->name(), collectionId0);
      ASSERT_TRUE((false == !logicalCollection0));
      logicalCollection1 = ci.getCollection(vocbase->name(), collectionId1);
      ASSERT_TRUE((false == !logicalCollection1));
      logicalView = ci.getView(vocbase->name(), viewId);
      ASSERT_TRUE((false == !logicalView));
      EXPECT_TRUE((false ==
                   logicalCollection0->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE(
          (true == logicalCollection1->getPhysical()->getAllIndexes().empty()));
      EXPECT_TRUE((false ==
                   logicalView->visitCollections(
                       [](arangodb::DataSourceId,
                          arangodb::LogicalView::Indexes*) { return false; })));
    }
  }
}

TEST_F(IResearchViewCoordinatorTest, IResearchViewNode_createBlock) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature

  createTestDatabase(vocbase);

  // create and drop view (no id specified)
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID

    EXPECT_TRUE((
        ci.createViewCoordinator(vocbase->name(), viewId, json->slice()).ok()));

    // get current plan version
    auto planVersion = arangodb::tests::getCurrentPlanVersion(server.server());

    auto view = ci.getView(vocbase->name(), viewId);
    ASSERT_TRUE(nullptr != view);
    ASSERT_TRUE(nullptr !=
                std::dynamic_pointer_cast<
                    arangodb::iresearch::IResearchViewCoordinator>(view));
    EXPECT_TRUE("testView" == view->name());
    EXPECT_TRUE(false == view->deleted());
    EXPECT_TRUE(viewId == std::to_string(view->id().id()));
    EXPECT_TRUE(arangodb::ViewType::kArangoSearch == view->type());
    EXPECT_TRUE(arangodb::LogicalDataSource::Category::kView ==
                view->category());
    EXPECT_TRUE(vocbase == &view->vocbase());

    // dummy query
    auto query = arangodb::aql::Query::create(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        arangodb::aql::QueryString(std::string_view("RETURN 1")), nullptr);
    arangodb::tests::waitForAsync(query->prepareQuery());

    arangodb::aql::SingletonNode singleton(query->plan(),
                                           arangodb::aql::ExecutionNodeId{0});

    arangodb::aql::Variable const outVariable("variable", 0, false,
                                              resourceMonitor);

    arangodb::iresearch::IResearchViewNode node(
        *query->plan(), arangodb::aql::ExecutionNodeId{42},
        *vocbase,  // database
        view,      // view
        outVariable,
        nullptr,  // no filter condition
        nullptr,  // no options
        {}        // no sort condition
    );
    node.addDependency(&singleton);

    singleton.setVarsUsedLater({arangodb::aql::VarSet{&outVariable}});
    singleton.setVarsValid({{}});
    node.setVarsUsedLater({{}});
    node.setVarsValid({arangodb::aql::VarSet{&outVariable}});
    singleton.setVarUsageValid();
    node.setVarUsageValid();
    singleton.planRegisters();
    node.planRegisters();
    auto singletonBlock = singleton.createBlock(*query->rootEngine());
    auto execBlock = node.createBlock(*query->rootEngine());
    ASSERT_TRUE(nullptr != execBlock);
    ASSERT_TRUE(nullptr !=
                dynamic_cast<arangodb::aql::ExecutionBlockImpl<
                    arangodb::aql::NoResultsExecutor>*>(execBlock.get()));

    // drop view
    EXPECT_TRUE(view->drop().ok());
    EXPECT_TRUE(planVersion <
                arangodb::tests::getCurrentPlanVersion(server.server()));

    // check there is no more view
    ASSERT_TRUE(nullptr == ci.getView(vocbase->name(), view->name()));
  }
}
