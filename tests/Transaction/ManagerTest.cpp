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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "Aql/Query.h"

#include "Aql/QueryAborter.h"
#include "Aql/SharedQueryState.h"
#include "Basics/ScopeGuard.h"
#include "Basics/debugging.h"
#include "Cluster/ServerState.h"
#include "Rest/GeneralResponse.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Manager.h"
#include "Transaction/SmartContext.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/Status.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Parser.h>

#include "gtest/gtest.h"

#include "ManagerSetup.h"
#include "../IResearch/common.h"

using namespace arangodb;

static arangodb::aql::QueryResult executeQuery(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<transaction::Context> ctx) {
  auto query = arangodb::aql::Query::create(
      ctx, arangodb::aql::QueryString(queryString), nullptr);

  arangodb::aql::QueryResult result;
  auto aborter = std::make_shared<arangodb::aql::QueryAborter>(query);
  while (true) {
    auto state = query->execute(aborter, result);
    if (state == arangodb::aql::ExecutionState::WAITING) {
      query->sharedState()->waitForAsyncWakeup();
    } else {
      break;
    }
  }

  return result;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class TransactionManagerTest : public ::testing::Test {
 protected:
  arangodb::tests::mocks::TransactionManagerSetup setup;
  TRI_vocbase_t vocbase;
  transaction::Manager* mgr;
  TransactionId tid;

  TransactionManagerTest()
      : vocbase(testDBInfo(setup.server.server())),
        mgr(transaction::ManagerFeature::manager()),
        tid(TransactionId::createLeader()) {}

  ~TransactionManagerTest() { mgr->garbageCollect(true); }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(TransactionManagerTest, parsing_errors) {
  auto json = arangodb::velocypack::Parser::fromJson("{ \"write\": [33] }");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  EXPECT_TRUE(res.is(TRI_ERROR_BAD_PARAMETER));

  json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": \"33\"}, \"lockTimeout\": -1 }");
  res = mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                              transaction::OperationOriginTestCase{}, false)
            .waitAndGet();
  EXPECT_TRUE(res.is(TRI_ERROR_BAD_PARAMETER));
}

TEST_F(TransactionManagerTest, collection_not_found) {
  arangodb::ExecContextSuperuserScope exeScope;

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"33\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  EXPECT_EQ(res.errorNumber(), TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);

  json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"33\"]}}");
  res = mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                              transaction::OperationOriginTestCase{}, false)
            .waitAndGet();
  EXPECT_EQ(res.errorNumber(), TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);

  json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"exclusive\": [\"33\"]}}");
  res = mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                              transaction::OperationOriginTestCase{}, false)
            .waitAndGet();
  EXPECT_EQ(res.errorNumber(), TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
}

TEST_F(TransactionManagerTest, transaction_id_reuse) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"33\"]}}");
  res = mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                              transaction::OperationOriginTestCase{}, false)
            .waitAndGet();
  EXPECT_EQ(res.errorNumber(), TRI_ERROR_TRANSACTION_INTERNAL);

  res = mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());
}

TEST_F(TransactionManagerTest, simple_transaction_and_abort) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  auto doc = arangodb::velocypack::Parser::fromJson("{ \"_key\": \"1\"}");
  {
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);
    auto origin = ctx->operationOrigin();
    ASSERT_EQ(transaction::OperationOrigin::Type::kInternal, origin.type);
    ASSERT_EQ("unit test", origin.description);

    SingleCollectionTransaction trx(ctx, "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(!trx.isMainTransaction());

    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }

  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);

  {  // lease again
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);

    SingleCollectionTransaction trx(ctx, "testCollection",
                                    AccessMode::Type::READ);
    ASSERT_TRUE(!trx.isMainTransaction());

    OperationOptions opts;
    auto opRes = trx.document(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // perform same operation
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // cannot commit aborted transaction
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name())
                  .waitAndGet()
                  .is(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION));

  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, simple_transaction_and_commit) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  {
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);

    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(!trx.isMainTransaction());
    ASSERT_FALSE(
        trx.state()->hasHint(transaction::Hints::Hint::IS_FOLLOWER_TRX));

    auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": 1}");

    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // perform same operation
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // cannot commit aborted transaction
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name())
                  .waitAndGet()
                  .is(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION));

  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::COMMITTED);
}

TEST_F(TransactionManagerTest, simple_transaction_and_commit_is_follower) {
  tid = TransactionId::createFollower();
  auto beforeRole = arangodb::ServerState::instance()->getRole();
  auto roleGuard = scopeGuard([&]() noexcept {
    arangodb::ServerState::instance()->setRole(beforeRole);
  });
  arangodb::ServerState::instance()->setRole(
      arangodb::ServerState::ROLE_DBSERVER);
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, true)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  {
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);

    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(!trx.isMainTransaction());
    ASSERT_TRUE(
        trx.state()->hasHint(transaction::Hints::Hint::IS_FOLLOWER_TRX));

    auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": 1}");

    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // perform same operation
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // cannot commit aborted transaction
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name())
                  .waitAndGet()
                  .is(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION));

  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::COMMITTED);
}

TEST_F(TransactionManagerTest, simple_transaction_and_commit_while_in_use) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  {
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);

    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(!trx.isMainTransaction());

    auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": 1}");

    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_EQ(
        TRI_ERROR_LOCKED,
        mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().errorNumber());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);

  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // perform same operation
  ASSERT_TRUE(mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  // cannot abort committed transaction
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name())
                  .waitAndGet()
                  .is(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION));
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::COMMITTED);
}

TEST_F(TransactionManagerTest, leading_multiple_readonly_transactions) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  {
    transaction::Options opts;
    bool responsible;

    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);
    auto state1 = ctx->acquireState(opts, responsible);
    ASSERT_NE(state1.get(), nullptr);
    ASSERT_TRUE(!responsible);

    auto ctx2 =
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, false).waitAndGet();
    ASSERT_NE(ctx2.get(), nullptr);
    auto state2 = ctx2->acquireState(opts, responsible);
    EXPECT_EQ(state1.get(), state2.get());
    ASSERT_TRUE(!responsible);

    auto ctx3 =
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, false).waitAndGet();
    ASSERT_NE(ctx3.get(), nullptr);
    auto state3 = ctx3->acquireState(opts, responsible);
    EXPECT_EQ(state3.get(), state2.get());
    ASSERT_TRUE(!responsible);
  }
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, lock_conflict) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());
  {
    transaction::Options opts;
    bool responsible;

    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);
    auto state1 = ctx->acquireState(opts, responsible);
    ASSERT_NE(state1.get(), nullptr);
    ASSERT_TRUE(!responsible);
    ASSERT_ANY_THROW(
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, false).waitAndGet());
  }
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, lock_conflict_side_user) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());
  {
    transaction::Options opts;
    bool responsible;

    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);
    auto state1 = ctx->acquireState(opts, responsible);
    ASSERT_NE(state1.get(), nullptr);
    ASSERT_TRUE(!responsible);
    ASSERT_ANY_THROW(
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, false).waitAndGet());

    auto ctxSide =
        mgr->leaseManagedTrx(tid, AccessMode::Type::READ, true).waitAndGet();
    ASSERT_NE(ctxSide.get(), nullptr);
    auto state2 = ctxSide->acquireState(opts, responsible);
    ASSERT_NE(state2.get(), nullptr);
    ASSERT_TRUE(!responsible);
  }
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, garbage_collection_shutdown) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());
  {
    transaction::Options opts;
    bool responsible;

    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);
    auto state1 = ctx->acquireState(opts, responsible);
    ASSERT_NE(state1.get(), nullptr);
    ASSERT_TRUE(!responsible);
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);
  ASSERT_TRUE(mgr->garbageCollect(/*abortAll*/ true));
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, aql_standalone_transaction) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  {
    auto ctx = transaction::StandaloneContext::create(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(trx.begin().ok());

    auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": 1}");
    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }

  auto ctx = std::make_shared<transaction::AQLStandaloneContext>(
      vocbase, tid,
      arangodb::transaction::OperationOriginAQL{"running AQL query"});
  auto origin = ctx->operationOrigin();
  ASSERT_EQ(transaction::OperationOrigin::Type::kAQL, origin.type);
  ASSERT_EQ("running AQL query", origin.description);

  auto qq = "FOR doc IN testCollection RETURN doc";
  arangodb::aql::QueryResult qres = executeQuery(vocbase, qq, ctx);
  ASSERT_TRUE(qres.ok());
  ASSERT_NE(nullptr, qres.data);
  VPackSlice data = qres.data->slice();
  ASSERT_TRUE(data.isArray());
  ASSERT_EQ(data.length(), 1);
  EXPECT_TRUE(data.at(0).isObject());
  EXPECT_TRUE(data.at(0).hasKey("abc"));
}

TEST_F(TransactionManagerTest, abort_transactions_with_matcher) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_TRUE(res.ok());

  {
    auto ctx =
        mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
    ASSERT_NE(ctx.get(), nullptr);

    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_TRUE(!trx.isMainTransaction());

    auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": 1}");
    OperationOptions opts;
    auto opRes = trx.insert(coll->name(), doc->slice(), opts);
    ASSERT_TRUE(opRes.ok());
    ASSERT_TRUE(trx.finish(opRes.result).ok());
  }
  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::RUNNING);

  mgr->abortManagedTrx(
      [](TransactionState const& state, std::string const& /*user*/) -> bool {
        TransactionCollection* tcoll =
            state.collection(DataSourceId{42}, AccessMode::Type::NONE);
        return tcoll != nullptr;
      });

  ASSERT_EQ(mgr->getManagedTrxStatus(tid), transaction::Status::ABORTED);
}

TEST_F(TransactionManagerTest, permission_denied_readonly) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  struct ExecContext : public arangodb::ExecContext {
    ExecContext()
        : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                arangodb::ExecContext::Type::Internal, "dummy",
                                "testVocbase", arangodb::auth::Level::RO,
                                arangodb::auth::Level::RO, false) {}
  };
  auto execContext = std::make_shared<ExecContext>();
  arangodb::ExecContextScope execContextScope(execContext);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  EXPECT_TRUE(res.ok());
  ASSERT_TRUE(mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet().ok());

  tid = TransactionId::createSingleServer();
  json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"42\"]}}");
  res = mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                              transaction::OperationOriginTestCase{}, false)
            .waitAndGet();
  ASSERT_EQ(res.errorNumber(), TRI_ERROR_ARANGO_READ_ONLY);
}

TEST_F(TransactionManagerTest, permission_denied_forbidden) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json =
        VPackParser::fromJson("{ \"name\": \"testCollection\", \"id\": 42 }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  struct ExecContext : public arangodb::ExecContext {
    ExecContext()
        : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                                arangodb::ExecContext::Type::Internal, "dummy",
                                "testVocbase", arangodb::auth::Level::NONE,
                                arangodb::auth::Level::NONE, false) {}
  };
  auto execContext = std::make_shared<ExecContext>();
  arangodb::ExecContextScope execContextScope(execContext);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"42\"]}}");
  Result res =
      mgr->ensureManagedTrx(vocbase, tid, json->slice(),
                            transaction::OperationOriginTestCase{}, false)
          .waitAndGet();
  ASSERT_EQ(res.errorNumber(), TRI_ERROR_FORBIDDEN);
}

TEST_F(TransactionManagerTest, transaction_invalid_mode) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json = VPackParser::fromJson("{ \"name\": \"testCollection\" }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"read\": [\"testCollection\"]}}");
  Result res = mgr->ensureManagedTrx(
                      vocbase, tid, json->slice(),
                      transaction::OperationOriginInternal{"some test"}, false)
                   .waitAndGet();
  ASSERT_TRUE(res.ok());

  auto ctx =
      mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
  ASSERT_NE(ctx.get(), nullptr);

  ASSERT_THROW(SingleCollectionTransaction(std::move(ctx), "testCollection",
                                           AccessMode::Type::WRITE),
               arangodb::basics::Exception);
  ;

  res = mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());
}

TEST_F(TransactionManagerTest, transaction_origin) {
  std::shared_ptr<LogicalCollection> coll;
  {
    auto json = VPackParser::fromJson("{ \"name\": \"testCollection\" }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"testCollection\"]}}");
  Result res = mgr->ensureManagedTrx(
                      vocbase, tid, json->slice(),
                      transaction::OperationOriginInternal{"some test"}, false)
                   .waitAndGet();
  ASSERT_TRUE(res.ok());

  auto ctx =
      mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet();
  ASSERT_NE(ctx.get(), nullptr);

  {
    SingleCollectionTransaction trx(std::move(ctx), "testCollection",
                                    AccessMode::Type::WRITE);
    ASSERT_FALSE(trx.isMainTransaction());
    auto origin = trx.state()->operationOrigin();
    ASSERT_EQ(transaction::OperationOrigin::Type::kInternal, origin.type);
    ASSERT_EQ("some test", origin.description);
  }

  res = mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());
}

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
TEST_F(TransactionManagerTest, expired_transaction) {
  auto guard = scopeGuard([]() noexcept { TRI_ClearFailurePointsDebugging(); });

  std::shared_ptr<LogicalCollection> coll;
  {
    auto json = VPackParser::fromJson("{ \"name\": \"testCollection\" }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"testCollection\"]}}");

  // disables garbage-collection
  TRI_AddFailurePointDebugging("transaction::Manager::noGC");
  // sets TTL for transaction to a very low value
  TRI_AddFailurePointDebugging("transaction::Manager::shortTTL");
  Result res = mgr->ensureManagedTrx(
                      vocbase, tid, json->slice(),
                      transaction::OperationOriginInternal{"some test"}, false)
                   .waitAndGet();
  ASSERT_TRUE(res.ok());

  // wait until trx is expired
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // we cannot use the transaction anymore
  ASSERT_ANY_THROW(
      mgr->leaseManagedTrx(tid, AccessMode::Type::WRITE, false).waitAndGet());

  // aborting it is fine though
  res = mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());

  res = mgr->commitManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_FALSE(res.ok());
  ASSERT_EQ(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION, res.errorNumber());
}
#endif

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
TEST_F(TransactionManagerTest, lock_usage_of_expired_transaction) {
  auto guard = scopeGuard([]() noexcept { TRI_ClearFailurePointsDebugging(); });

  std::shared_ptr<LogicalCollection> coll;
  {
    auto json = VPackParser::fromJson("{ \"name\": \"testCollection\" }");
    coll = vocbase.createCollection(json->slice());
  }
  ASSERT_NE(coll, nullptr);

  auto json1 = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"exclusive\": [\"testCollection\"]}}");

  // sets TTL for transaction to a very low value
  TRI_AddFailurePointDebugging("transaction::Manager::shortTTL");
  Result res = mgr->ensureManagedTrx(
                      vocbase, tid, json1->slice(),
                      transaction::OperationOriginInternal{"some test"}, false)
                   .waitAndGet();
  ASSERT_TRUE(res.ok());

  // wait until trx is expired
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // we must now be able to open a second transaction on the
  // underlying collection, at least after the garbage collection
  mgr->garbageCollect(/*abortAll*/ false);

  TRI_ClearFailurePointsDebugging();

  TransactionId tid2 = TransactionId::createLeader();
  auto json2 = arangodb::velocypack::Parser::fromJson(
      "{ \"collections\":{\"write\": [\"testCollection\"]}}");
  res = mgr->ensureManagedTrx(vocbase, tid2, json2->slice(),
                              transaction::OperationOriginInternal{"some test"},
                              false)
            .waitAndGet();
  ASSERT_TRUE(res.ok());

  // aborting trx1 is still fine, even though it is expired
  res = mgr->abortManagedTrx(tid, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());

  // committing trx2 must be ok
  res = mgr->commitManagedTrx(tid2, vocbase.name()).waitAndGet();
  ASSERT_TRUE(res.ok());
}
#endif
