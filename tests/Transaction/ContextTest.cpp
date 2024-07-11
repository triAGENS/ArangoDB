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
#include "Rest/GeneralResponse.h"
#include "Transaction/Manager.h"
#include "Transaction/SmartContext.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/Status.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

#include "ManagerSetup.h"

#include <velocypack/Builder.h>
#include <velocypack/Parser.h>

#include "../IResearch/common.h"
#include "gtest/gtest.h"

using namespace arangodb;

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

/// @brief test transaction::Context
class TransactionContextTest : public ::testing::Test {
 protected:
  arangodb::tests::mocks::TransactionManagerSetup setup;
  TRI_vocbase_t vocbase;

  TransactionContextTest() : vocbase(testDBInfo(setup.server.server())) {}
};

TEST_F(TransactionContextTest, StandaloneContext) {
  transaction::StandaloneContext ctx(vocbase,
                                     transaction::OperationOriginTestCase{});
  EXPECT_TRUE(ctx.isEmbeddable());
  EXPECT_FALSE(ctx.isStateSet());

  std::vector<std::string*> strings;
  for (int i = 0; i < 10; i++) {
    std::string* str = ctx.leaseString();
    if (i > 0) {
      EXPECT_NE(strings.back(), str);
    }
    strings.push_back(str);
  }

  while (!strings.empty()) {
    std::string* str = strings.back();
    ctx.returnString(str);
    strings.pop_back();
  }

  std::vector<velocypack::Builder*> builders;
  for (int i = 0; i < 10; i++) {
    auto* b = ctx.leaseBuilder();
    if (i > 0) {
      EXPECT_NE(builders.back(), b);
    }
    builders.push_back(b);
  }
  while (!builders.empty()) {
    auto* b = builders.back();
    ctx.returnBuilder(b);
    builders.pop_back();
  }
}

TEST_F(TransactionContextTest, StandaloneSmartContext) {
  auto const cname = "testCollection";
  auto params = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection\" }");
  vocbase.createCollection(params->slice());

  auto ctx = std::make_shared<transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  transaction::Options trxOpts;
  transaction::Methods trx{
      ctx, {}, std::vector<std::string>{cname}, {}, trxOpts};

  Result res = trx.begin();
  ASSERT_TRUE(res.ok());

  auto docs = arangodb::velocypack::Parser::fromJson(
      "[{ \"hello\": \"world1\" }, { \"hello\": \"world2\" }]");

  OperationOptions opOpts;
  OperationResult result = trx.insert(cname, docs->slice(), opOpts);
  ASSERT_TRUE(result.ok());

  VPackSlice trxSlice = result.slice();
  ASSERT_TRUE(trxSlice.isArray());
  ASSERT_EQ(trxSlice.length(), 2);

  aql::QueryString queryString{
      std::string_view("FOR doc IN @@collection FILTER doc.hello != '' SORT "
                       "doc.hello RETURN doc")};

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->add(VPackValue(VPackValueType::Object));
  bindVars->add("@collection", VPackValue(cname));
  bindVars->close();

  {
    auto query = arangodb::aql::Query::create(ctx, queryString, bindVars);

    auto aborter = std::make_shared<arangodb::aql::QueryAborter>(query);
    auto qres = query->executeSync(aborter);
    ASSERT_TRUE(qres.ok());
    ASSERT_NE(nullptr, qres.data);
    VPackSlice aqlSlice = qres.data->slice();
    ASSERT_TRUE(aqlSlice.isArray());
    ASSERT_EQ(aqlSlice.length(), 2);
    ASSERT_TRUE(aqlSlice.at(0).get("hello").isEqualString("world1"));
  }

  ASSERT_TRUE(trxSlice.at(1).hasKey(StaticStrings::KeyString));
  OperationResult result2 = trx.remove(cname, trxSlice.at(0), opOpts);
  ASSERT_TRUE(result2.ok());

  {
    auto query = arangodb::aql::Query::create(ctx, queryString, bindVars);

    auto aborter = std::make_shared<arangodb::aql::QueryAborter>(query);
    auto qres = query->executeSync(aborter);
    ASSERT_TRUE(qres.ok());
    ASSERT_NE(nullptr, qres.data);
    VPackSlice aqlSlice = qres.data->slice();
    ASSERT_TRUE(aqlSlice.isArray());
    ASSERT_EQ(aqlSlice.length(), 1);
    ASSERT_TRUE(aqlSlice.at(0).get("hello").isEqualString("world2"));
  }
}  // SECTION("StandaloneSmartContext")
