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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>

// test setup
#include "IResearch/common.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"
#include "Mocks/StorageEngineMock.h"

#include "Aql/AqlFunctionFeature.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/SharedQueryState.h"
#include "Basics/VelocyPackHelper.h"
#include "ClusterEngine/ClusterEngine.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "Random/RandomGenerator.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/OperationOrigin.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

extern const char* ARGV0;  // defined in main.cpp

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

// The Paramater is a Flag if we activate fullcount or not.

class SortLimitTest
    : public ::testing::TestWithParam<bool>,
      public arangodb::tests::LogSuppressor<arangodb::Logger::FIXME,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;
  std::vector<
      std::pair<arangodb::application_features::ApplicationFeature&, bool>>
      features;

  std::unique_ptr<TRI_vocbase_t> vocbase;
  std::vector<arangodb::velocypack::Builder> insertedDocs;

  SortLimitTest() : server() {
    arangodb::transaction::Methods::clearDataSourceRegistrationCallbacks();
    arangodb::ClusterEngine::Mocking = true;
    arangodb::RandomGenerator::initialize(
        arangodb::RandomGenerator::RandomType::MERSENNE);

    vocbase = std::make_unique<TRI_vocbase_t>(

        testDBInfo(server.server()));

    CreateCollection();
  }

  ~SortLimitTest() { vocbase.reset(); }

  auto doFullCount() -> bool { return GetParam(); }

  auto buildOptions(std::string rules)
      -> std::shared_ptr<arangodb::velocypack::Builder> {
    if (doFullCount()) {
      return arangodb::velocypack::Parser::fromJson(
          "{\"optimizer\": {\"rules\": [" + rules + "]}, \"fullCount\": true}");
    }
    return arangodb::velocypack::Parser::fromJson(
        "{\"optimizer\": {\"rules\": [" + rules + "]}}");
  }

  std::string sorterType(TRI_vocbase_t& vocbase, std::string const& queryString,
                         std::string rules = "") {
    auto options = buildOptions(rules);
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto query = arangodb::aql::Query::create(
        ctx, arangodb::aql::QueryString(queryString), nullptr,
        arangodb::aql::QueryOptions(options->slice()));

    auto result = query->explain();
    VPackSlice nodes = result.data->slice().get("nodes");
    EXPECT_TRUE(nodes.isArray());

    std::string strategy;
    for (VPackSlice it : VPackArrayIterator(nodes)) {
      if (!it.get("type").isEqualString("SortNode")) {
        continue;
      }

      EXPECT_TRUE(strategy.empty());
      strategy = it.get("strategy").copyString();
    }

    EXPECT_FALSE(strategy.empty());
    return strategy;
  }

  void verifyExpectedResults(TRI_vocbase_t& vocbase,
                             std::string const& queryString,
                             std::vector<size_t> const& expected,
                             size_t fullCount, std::string rules = "") {
    auto options = buildOptions(rules);
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase, arangodb::transaction::OperationOriginTestCase{});
    auto query = arangodb::aql::Query::create(
        ctx, arangodb::aql::QueryString(queryString), nullptr,
        arangodb::aql::QueryOptions(options->slice()));
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

    EXPECT_TRUE(result.result.ok());
    auto slice = result.data->slice();
    EXPECT_TRUE(slice.isArray());
    ASSERT_EQ(slice.length(), expected.size());

    size_t i = 0;
    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();

      EXPECT_EQ(0, arangodb::basics::VelocyPackHelper::compare(
                       insertedDocs[expected[i]].slice(), resolved, true))
          << insertedDocs[expected[i]].slice().toJson() << " vs. "
          << resolved.toJson();
      i++;
    }
    auto actualFullCount =
        arangodb::basics::VelocyPackHelper::getNumericValue<size_t>(
            result.extra->slice().get(
                std::vector<std::string_view>{"stats", "fullCount"}),
            0);
    if (doFullCount()) {
      EXPECT_EQ(actualFullCount, fullCount);
    } else {
      EXPECT_EQ(actualFullCount, 0);
    }
  }

  // create collection0, insertedDocs[0, 999]
  void CreateCollection() {
    auto createJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection0\" }");
    auto collection = vocbase->createCollection(createJson->slice());
    ASSERT_NE(nullptr, collection);

    std::vector<std::shared_ptr<arangodb::velocypack::Builder>> docs;
    size_t total = 1000;
    for (size_t i = 0; i < total; i++) {
      docs.emplace_back(arangodb::velocypack::Parser::fromJson(
          "{ \"valAsc\": " + std::to_string(i) +
          ", \"valDsc\": " + std::to_string(total - 1 - i) +
          ", \"mod\": " + std::to_string(i % 100) + "}"));
    }

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        *collection, arangodb::AccessMode::Type::WRITE);
    EXPECT_TRUE(trx.begin().ok());

    for (auto& entry : docs) {
      auto res = trx.insert(collection->name(), entry->slice(), options);
      EXPECT_TRUE(res.ok());
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    EXPECT_TRUE(trx.commit().ok());
    EXPECT_EQ(insertedDocs.size(), total);
  }
};

INSTANTIATE_TEST_CASE_P(SortLimitTest, SortLimitTest, ::testing::Bool());

TEST_P(SortLimitTest, CheckSimpleLimitSortedAscInInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc LIMIT 0, 10 RETURN d";
  std::vector<size_t> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetSortedAscInInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc LIMIT 10, 10 RETURN d";
  std::vector<size_t> expected = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckSimpleLimitSortedAscInReverseInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valDsc LIMIT 0, 10 RETURN d";
  std::vector<size_t> expected = {999, 998, 997, 996, 995,
                                  994, 993, 992, 991, 990};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetSortedAscInReverseInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valDsc LIMIT 10, 10 RETURN d";
  std::vector<size_t> expected = {989, 988, 987, 986, 985,
                                  984, 983, 982, 981, 980};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckSimpleLimitSortedDscInInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc DESC LIMIT 0, 10 RETURN d";
  std::vector<size_t> expected = {999, 998, 997, 996, 995,
                                  994, 993, 992, 991, 990};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetSortedDscInInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc DESC LIMIT 10, 10 RETURN d";
  std::vector<size_t> expected = {989, 988, 987, 986, 985,
                                  984, 983, 982, 981, 980};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckSimpleLimitSortedDscInReverseInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valDsc DESC LIMIT 0, 10 RETURN d";
  std::vector<size_t> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetSortedDscInReverseInsertionOrder) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valDsc DESC LIMIT 10, 10 RETURN d";
  std::vector<size_t> expected = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetCompoundSort) {
  std::string query =
      "FOR d IN testCollection0 SORT d.mod, d.valAsc LIMIT 2, 5 RETURN  d";
  std::vector<size_t> expected = {200, 300, 400, 500, 600};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckLimitWithOffsetCompoundSortAgain) {
  std::string query =
      "FOR d IN testCollection0 SORT d.mod, d.valAsc LIMIT 10, 10 RETURN d";
  std::vector<size_t> expected = {1,   101, 201, 301, 401,
                                  501, 601, 701, 801, 901};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}

TEST_P(SortLimitTest, CheckInterloperFilterMovedUp) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc FILTER d.mod == 0 LIMIT 0, 10 "
      "RETURN d";
  std::vector<size_t> expected = {0,   100, 200, 300, 400,
                                  500, 600, 700, 800, 900};
  EXPECT_EQ(sorterType(*vocbase, query), "constrained-heap");
  verifyExpectedResults(*vocbase, query, expected, 10);
}

TEST_P(SortLimitTest, CheckInterloperFilterNotMoved) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc FILTER d.mod == 0 LIMIT 0, 10 "
      "RETURN d";
  std::string rules = "\"-move-filters-up\", \"-move-filters-up-2\"";
  std::vector<size_t> expected = {0,   100, 200, 300, 400,
                                  500, 600, 700, 800, 900};
  EXPECT_EQ(sorterType(*vocbase, query, rules), "standard");
  verifyExpectedResults(*vocbase, query, expected, 10, rules);
}

TEST_P(SortLimitTest, CheckInterloperEnumerateList) {
  std::string query =
      "FOR d IN testCollection0 SORT d.valAsc FOR e IN 1..10 FILTER e == 1 "
      "LIMIT 0, 10 RETURN d";
  std::vector<size_t> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(sorterType(*vocbase, query), "standard");
  verifyExpectedResults(*vocbase, query, expected, 1000);
}
