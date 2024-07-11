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

#include "gtest/gtest.h"

#include "QueryHelper.h"

#include "Aql/Ast.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionNode/SubqueryEndExecutionNode.h"
#include "Aql/ExecutionNode/SubqueryStartExecutionNode.h"
#include "Aql/WalkerWorker.h"
#include "Logger/LogMacros.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"

#include "velocypack/Builder.h"
#include "velocypack/Collection.h"
#include "velocypack/Slice.h"

#include "../IResearch/IResearchQueryCommon.h"
#include "../Mocks/Servers.h"

#include <string_view>

using namespace arangodb::aql;
using namespace arangodb::containers;

namespace {
struct Comparator final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
  Comparator(Comparator const&) = delete;
  Comparator& operator=(Comparator const&) = delete;

  Comparator(ExecutionPlan* other, bool expectNormalSubqueries)
      : _other(other), _expectNormalSubqueries(expectNormalSubqueries) {}

  static bool isSubqueryRelated(ExecutionNode* en) {
    return en->getType() == ExecutionNode::SUBQUERY ||
           en->getType() == ExecutionNode::SUBQUERY_START ||
           en->getType() == ExecutionNode::SUBQUERY_END ||
           en->getType() == ExecutionNode::SINGLETON;
  }

  bool before(ExecutionNode* en) override final {
    std::set<ExecutionNodeId> depids, otherdepids;

    if (!isSubqueryRelated(en)) {
      try {
        auto otherNode = _other->getNodeById(en->id());
        auto otherDeps = otherNode->getDependencies();

        for (auto d : otherDeps) {
          if (!isSubqueryRelated(d)) {
            otherdepids.insert(d->id());
          }
        }

        for (auto d : en->getDependencies()) {
          if (!isSubqueryRelated(d)) {
            depids.insert(d->id());
          }
        }

        EXPECT_EQ(depids, otherdepids);
      } catch (...) {
        EXPECT_TRUE(false) << "expected node with id " << en->id().id()
                           << " of type " << en->getTypeString()
                           << " to be present in optimized plan";
      }
    }
    return false;
  }

  void after(ExecutionNode* en) override final {}

  bool enterSubquery(ExecutionNode*, ExecutionNode* sub) override final {
    EXPECT_TRUE(_expectNormalSubqueries)
        << "Optimized plan must not contain SUBQUERY nodes";
    return false;
  }

 private:
  ExecutionPlan* _other{};
  bool _expectNormalSubqueries{};
};

}  // namespace

namespace arangodb {
namespace tests {
namespace aql {

class SpliceSubqueryNodeOptimizerRuleTest : public ::testing::Test {
 protected:
  mocks::MockAqlServer server;
  QueryRegistry* queryRegistry{QueryRegistryFeature::registry()};

  std::shared_ptr<VPackBuilder> ruleOptions(
      char const* additionalOptionsString) const {
    return VPackParser::fromJson(additionalOptionsString);
  }

  void verifySubquerySplicing(std::string const& querystring,
                              size_t const expectedNumberOfSplicedSubqueries,
                              char const* bindParameters = "{}",
                              char const* additionalOptions = "{}") {
    auto const expectedNumberOfSubqueries = expectedNumberOfSplicedSubqueries;
    ASSERT_NE(queryRegistry, nullptr) << "query string: " << querystring;
    ASSERT_EQ(queryRegistry->numberRegisteredQueries(), 0)
        << "query string: " << querystring;

    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        server.getSystemDatabase(), transaction::OperationOriginTestCase{});
    auto const bindParamVpack = VPackParser::fromJson(bindParameters);
    auto splicedQuery = arangodb::aql::Query::create(
        std::move(ctx), arangodb::aql::QueryString(querystring), bindParamVpack,
        arangodb::aql::QueryOptions(ruleOptions(additionalOptions)->slice()));
    auto aborter = std::make_shared<arangodb::aql::QueryAborter>(splicedQuery);
    splicedQuery->prepareQuery(aborter);
    ASSERT_EQ(queryRegistry->numberRegisteredQueries(), 0)
        << "query string: " << querystring;

    auto splicedPlan =
        const_cast<arangodb::aql::ExecutionPlan*>(splicedQuery->plan());
    ASSERT_NE(splicedPlan, nullptr) << "query string: " << querystring;

    containers::SmallVector<ExecutionNode*, 8> splicedSubqueryNodes;
    containers::SmallVector<ExecutionNode*, 8> splicedSubqueryStartNodes;
    containers::SmallVector<ExecutionNode*, 8> splicedSubqueryEndNodes;
    containers::SmallVector<ExecutionNode*, 8> splicedSubquerySingletonNodes;
    splicedPlan->findNodesOfType(splicedSubqueryNodes, ExecutionNode::SUBQUERY,
                                 true);
    splicedPlan->findNodesOfType(splicedSubqueryStartNodes,
                                 ExecutionNode::SUBQUERY_START, true);
    splicedPlan->findNodesOfType(splicedSubqueryEndNodes,
                                 ExecutionNode::SUBQUERY_END, true);

    splicedPlan->findNodesOfType(splicedSubquerySingletonNodes,
                                 ExecutionNode::SINGLETON, true);

    EXPECT_EQ(expectedNumberOfSplicedSubqueries,
              splicedSubqueryStartNodes.size())
        << "query string: " << querystring;
    EXPECT_EQ(expectedNumberOfSplicedSubqueries, splicedSubqueryEndNodes.size())
        << "query string: " << querystring;

    EXPECT_EQ(splicedSubquerySingletonNodes.size(), 1)
        << "query string: " << querystring;

    // Make sure no nodes got lost (currently does not check SubqueryNodes,
    // SubqueryStartNode, SubqueryEndNode correctness)
    Comparator compare(splicedPlan, expectedNumberOfSubqueries > 0);
    splicedPlan->root()->walk(compare);
  }

  void verifyQueryResult(std::string const& query, VPackSlice const expected,
                         char const* bindParameters = "{}",
                         char const* additionalOptions = "{}") {
    auto const bindParamVPack = VPackParser::fromJson(bindParameters);
    SCOPED_TRACE("Query: " + query);

    {
      auto queryResult = arangodb::tests::executeQuery(
          server.getSystemDatabase(), query, bindParamVPack,
          ruleOptions(additionalOptions)->toJson());
      SCOPED_TRACE("rule was enabled");
      AssertQueryResultToSlice(queryResult, expected);
    }
  }

 public:
  SpliceSubqueryNodeOptimizerRuleTest() {}

};  // namespace aql

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_no_subquery_plan) {
  auto query = "RETURN 15";
  verifySubquerySplicing(query, 0);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([15])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_single_input) {
  auto query = R"aql(FOR d IN 1..1
                      LET first =
                        (RETURN 1)
                      RETURN first)aql";
  verifySubquerySplicing(query, 1);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([[1]])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_plan) {
  auto query = R"aql(FOR d IN 1..2
                      LET first =
                        (FOR e IN 1..2 FILTER d == e RETURN e)
                      RETURN first)aql";
  verifySubquerySplicing(query, 1);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([[1],[2]])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_in_subquery_plan) {
  auto query = R"aql(FOR d IN 1..2
                        LET first = (FOR e IN 1..2
                                        LET second = (FOR f IN 1..2 RETURN f)
                                        FILTER d == e RETURN [e, second])
                         RETURN first)aql";
  verifySubquerySplicing(query, 2);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([
    [[1, [1,2]]],
    [[2, [1,2]]]
  ])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_after_subquery_plan) {
  auto query = R"aql(FOR d IN 1..2
                        LET first = (FOR e IN 1..2 FILTER d == e RETURN e)
                        LET second = (FOR e IN 1..2 FILTER d != e RETURN e)
                        RETURN [first, second])aql";
  verifySubquerySplicing(query, 2);

  auto expected =
      arangodb::velocypack::Parser::fromJson(R"([[[1],[2]],[[2],[1]]])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_with_sort) {
  auto query = R"aql(
    FOR i IN 1..2
      LET sorted = (
        FOR k IN [1,5,3,2,4,6]
          LET h = k * i
          SORT h DESC
        RETURN h
      )
      RETURN sorted)aql";
  verifySubquerySplicing(query, 1);

  auto expected = arangodb::velocypack::Parser::fromJson(
      R"([[6,5,4,3,2,1], [12,10,8,6,4,2]])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_skip__inner_limit_offset) {
  auto const queryString = R"aql(FOR i IN 0..2
    LET a = (FOR j IN 0..2 LIMIT 1, 1 RETURN 3*i + j)
    RETURN FIRST(a))aql";
  auto const expectedString = R"res([1, 4, 7])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_skip__outer_limit_offset) {
  auto const queryString = R"aql(FOR i IN 0..2
    LET a = (FOR j IN 0..2 RETURN 3*i + j)
    LIMIT 1, 1
    RETURN FIRST(a))aql";
  auto const expectedString = R"res([3])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_skip__inner_collect_count) {
  auto const queryString = R"aql(FOR i IN 0..2
    LET a = (FOR j IN 0..i COLLECT WITH COUNT INTO n RETURN n)
    RETURN FIRST(a))aql";
  auto const expectedString = R"res([1, 2, 3])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_skip__outer_collect_count) {
  // the RAND() is there to avoid the subquery being removed
  auto const queryString = R"aql(FOR i IN 0..2
    LET a = (FOR j IN 0..FLOOR(2*RAND()) RETURN 1)
    COLLECT WITH COUNT INTO n
    RETURN n)aql";
  auto const expectedString = R"res([3])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_skip__full_count) {
  // the RAND() is there to avoid the subquery being removed
  auto const queryString = R"aql(FOR i IN 0..2
    LET a = (FOR j IN 0..FLOOR(2*RAND()) RETURN 1)
    LIMIT 1
    RETURN i)aql";
  auto const expectedString = R"res([0])res";

  verifySubquerySplicing(queryString, 1, "{}",
                         R"opts({"fullCount": true})opts");
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_nested_subquery) {
  auto const queryString = R"aql(
    FOR i IN 0..1
    LET js = ( // this subquery should be spliced
      FOR j IN 0..1 + FLOOR(RAND())
      LET ks = ( // this subquery should be spliced
        FOR k IN 0..1 + FLOOR(RAND())
        RETURN 4*i + 2*j + k
      )
      RETURN ks
    )
    RETURN js
  )aql";
  auto const expectedString = R"res([[[0, 1], [2, 3]], [[4, 5], [6, 7]]])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_nested_subquery_with_innermost_skip) {
  auto const queryString = R"aql(
    FOR i IN 0..1
    LET js = ( // this subquery should be spliced
      FOR j IN 0..1 + FLOOR(RAND())
      LET ks = ( // this subquery should be spliced
        FOR k IN 0..2 + FLOOR(RAND())
        LIMIT 1, 2
        RETURN 6*i + 3*j + k
      )
      RETURN ks
    )
    RETURN js
  )aql";
  auto const expectedString = R"res([[[1, 2], [4, 5]], [[7, 8], [10, 11]]])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it. This is a test for a specific problem where constant subqueries did not
// work inside spliced subqueries correctly.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_nested_subquery_with_innermost_constant_skip) {
  auto const queryString = R"aql(
    FOR a IN 1..2
      LET b = (RETURN (FOR c IN 2..4 LIMIT 1, null RETURN c) )
    RETURN {a, b}
  )aql";
  auto const expectedString =
      R"res([{"a": 1, "b": [[3, 4]]}, {"a": 2, "b": [[3, 4]]}])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_nested_subquery_with_outermost_skip) {
  auto const queryString = R"aql(
    FOR i IN 0..2
    LET js = ( // this subquery should be spliced
      FOR j IN 0..1 + FLOOR(RAND())
      LET ks = ( // this subquery should be spliced
        FOR k IN 0..1 + FLOOR(RAND())
        RETURN 4*i + 2*j + k
      )
      RETURN ks
    )
    LIMIT 1, 2
    RETURN js
  )aql";
  auto const expectedString = R"res([[[4, 5], [6, 7]], [[8, 9], [10, 11]]])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Must be changed as soon as the subquery implementation with shadow rows
// handle skipping, and the splice-subqueries optimizer rule is changed to allow
// it.
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_limit_and_no_offset) {
  auto query = R"aql(
    FOR i IN 2..4
      LET a = (FOR j IN [i, i+10, i+20] LIMIT 0, 1 RETURN j)
      RETURN FIRST(a))aql";
  verifySubquerySplicing(query, 1);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([2, 3, 4])");
  verifyQueryResult(query, expected->slice());
}

// Regression test for https://github.com/arangodb/arangodb/issues/10852
TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_nested_empty_subqueries) {
  auto const queryString = R"aql(
    LET results = (
        FOR doc IN []
            LET docs = (
                FOR doc2 in []
                    RETURN doc2
            )
            RETURN [docs]
    )

    RETURN [results]
  )aql";
  auto const expectedString = R"res([[[]]])res";
  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

// Regression test for https://github.com/arangodb/arangodb/issues/10852
TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_with_upsert) {
  TRI_vocbase_t& vocbase{server.getSystemDatabase()};
  auto const info = VPackParser::fromJson(R"({"name":"UnitTestCollection"})");
  auto const collection = vocbase.createCollection(info->slice());
  auto const queryString = R"aql(
    LET new_id = (
        UPSERT { _key: @key }
        INSERT { _key: @key }
        UPDATE { _key: @key }
        IN UnitTestCollection
        RETURN NEW._id
    )
    RETURN new_id
  )aql";

  auto const bindString = R"bind({"key": "myKey"})bind";
  auto const expectedString = R"res([["UnitTestCollection/myKey"]])res";

  verifySubquerySplicing(queryString, 2, bindString);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice(), bindString);

  auto const noCollections = std::vector<std::string>{};
  auto const readCollection = std::vector<std::string>{"UnitTestCollection"};
  transaction::Options opts;
  auto ctx = transaction::StandaloneContext::create(
      server.getSystemDatabase(),
      arangodb::transaction::OperationOriginTestCase{});
  auto trx = std::make_unique<arangodb::transaction::Methods>(
      std::move(ctx), readCollection, noCollections, noCollections, opts);
  ASSERT_EQ(1, collection->getPhysical()->numberDocuments(trx.get()));
  bool called = false;
  auto cb = [&](LocalDocumentId, arangodb::aql::DocumentData&&,
                VPackSlice document) {
    called = true;
    EXPECT_TRUE(document.isObject());
    EXPECT_TRUE(document.get("_key").isString());
    EXPECT_EQ(std::string{"myKey"}, document.get("_key").copyString());
    return true;
  };
  auto result = collection->getPhysical()->lookup(
      trx.get(), std::string_view{"myKey"}, cb, {});
  ASSERT_TRUE(called);
  ASSERT_TRUE(result.ok());
}

// Regression test for https://github.com/arangodb/arangodb/issues/10896
TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_before_collect) {
  auto const queryString = R"aql(
    LET tokens = (
      FOR word IN TOKENS("Some Text", "text_en")
        FILTER word IN TOKENS("Some Other Thing", "text_en")
        RETURN word
    )
    COLLECT token = tokens INTO bucket
    RETURN bucket[0]
  )aql";

  auto const expectedString = R"res([ {"tokens": ["some"]} ])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * `COLLECT ... INTO group` should not collect any variables declared outside
 * its containing subquery, especially not from subqueries before it.
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_collect_after_spliced_subquery) {
  auto const queryString = R"aql(
    FOR x IN 0..2
      LET is = (FOR i IN 0..2 RETURN i + 3*x)
      LET ys = (
        FOR y IN 0..x
          COLLECT p = 1 INTO group
          RETURN {x, is, group}
      )
      RETURN {ys}
  )aql";

  auto const expectedString = R"res([
    {"ys": [ {"x": 0, "is": [0, 1, 2], "group": [{"y": 0}] } ] },
    {"ys": [ {"x": 1, "is": [3, 4, 5], "group": [{"y": 0}, {"y": 1}] } ] },
    {"ys": [ {"x": 2, "is": [6, 7, 8], "group": [{"y": 0}, {"y": 1}, {"y": 2}] } ] }
  ])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * `COLLECT ... INTO group` should not collect any variables declared outside
 * its containing subquery, especially not from subqueries before it.
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_collect_after_spliced_subquery_with_collect) {
  auto const queryString = R"aql(
    FOR x IN 0..2
      LET is = (
        LET f = "foo"
        FOR i IN 0..x
          COLLECT p = 1 INTO igroup
          RETURN {x, igroup}
      )
      LET ys = (
        LET b = "bar"
        FOR y IN 0..x
          COLLECT p = 1 INTO ygroup
          RETURN {x, is, ygroup}
      )
      RETURN {ys}
  )aql";

  auto const expectedString = R"res([
    {"ys": [{
          "x": 0,
          "is": [{"x": 0, "igroup": [{"i": 0}]}],
          "ygroup": [{"y": 0}]
        }]},
    {"ys": [{
          "x": 1,
          "is": [{"x": 1, "igroup": [{"i": 0}, {"i": 1}]}],
          "ygroup": [{"y": 0}, {"y": 1}]
        }]},
    {"ys": [{
          "x": 2,
          "is": [{"x": 2, "igroup": [{"i": 0}, {"i": 1}, {"i": 2}]}],
          "ygroup": [{"y": 0}, {"y": 1}, {"y": 2}]
        }]}
  ])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * Inside a subquery a relative top-level collect should behave correctly (and
 * collect top level variables)
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_relative_top_level_collect) {
  auto const queryString = R"aql(
    LET y = (
      LET x = 12
      LET y = (FOR m IN 1..3 COLLECT mm = m INTO bar RETURN mm)
      COLLECT p = 1 INTO groups
      RETURN groups
    )
    RETURN y
  )aql";

  auto const expectedString = R"res([
    [
      [
        {"x":12, "y":[1, 2, 3]}
      ]
    ]
  ])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * Subqueries at top-level should not change the behavior of top-level collect
 * (and should not captures variables within these subqueries)
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_nested_queries_and_top_level_collect) {
  auto const queryString = R"aql(
    LET y = (
      LET z = (FOR m IN 1..3 COLLECT mm = m INTO bar RETURN mm)
      RETURN z
    )
    COLLECT yy = y INTO bar
    RETURN bar
  )aql";

  auto const expectedString = R"res([
    [{
      "y": [[1, 2, 3]]
    }]
  ])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * Subqueries at top-level should not change the behavior of top-level collect
 * (and should not captures variables within these subqueries)
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_nested_queries_and_collect_in_nested_subquery) {
  auto const queryString = R"aql(
    LET foo = (
        LET y = ( FOR x IN 1..6 RETURN x )
        FOR z IN 0..2
            RETURN y[2*z]
    )

    LET bar = (
        LET v = 13
        LET y = (
          LET k = 12
          FOR x IN foo
            COLLECT p = 0 INTO bar
            RETURN bar
        )
        RETURN y
    )

    RETURN bar
  )aql";

  auto const expectedString = R"res([
    [[
      [
        {"x":1}, {"x":3}, {"x":5}
      ]
    ]]
  ])res";

  verifySubquerySplicing(queryString, 4);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * A subquery with a collect should not affect a collect outside of that
 * subquery.
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_collect_and_outside_collect) {
  auto const queryString = R"aql(
    LET k = 12
    FOR x IN 0..5
        LET y = (FOR y IN 0..x COLLECT p = y % 2 WITH COUNT INTO n RETURN n)
        COLLECT p = y[0] == y[1] INTO bar
        RETURN bar
  )aql";

  auto const expectedString = R"res([
    [
      {"x":0, "y":[1]}, {"x":2, "y":[2, 1]}, {"x":4, "y":[3, 2]}
    ],
    [
      {"x":1, "y":[1, 1]}, {"x":3, "y":[2, 2]}, {"x":5, "y":[3, 3]}
    ]
  ])res";

  verifySubquerySplicing(queryString, 1);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

/*
 * A collect in a subquery should not collect top-level variables.
 */
TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_collect_in_subquery) {
  auto const queryString = R"aql(
    LET k = 14
    LET x = (
        LET y = (FOR x IN 1..6 FILTER 1 == x % 2 RETURN 2*x)
        FOR x IN 1..3
            COLLECT p = y[x] % 4 INTO baz
            RETURN baz
    )

    RETURN x
  )aql";

  auto const expectedString = R"res([
    [
      [{"x": 3}],
      [{"x": 1}, {"x": 2}]
    ]
  ])res";

  verifySubquerySplicing(queryString, 2);
  auto expected = arangodb::velocypack::Parser::fromJson(expectedString);
  verifyQueryResult(queryString, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_with_limit_and_offset) {
  auto query = R"aql(
    FOR i IN 2..4
      LET a = (FOR j IN [0, i, i+10] LIMIT 1, 1 RETURN j)
      RETURN FIRST(a))aql";
  verifySubquerySplicing(query, 1);

  auto expected = arangodb::velocypack::Parser::fromJson(R"([2, 3, 4])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest,
       splice_subquery_collect_within_empty_nested_subquery) {
  auto query = R"aql(
    FOR k IN 1..2
      LET sub1 = (
        FOR x IN []
          LET sub2 = (
            COLLECT WITH COUNT INTO q
            RETURN q
          )
        RETURN sub2
      )
    RETURN [k, sub1])aql";
  verifySubquerySplicing(query, 2);

  auto expected =
      arangodb::velocypack::Parser::fromJson(R"([[1, []], [2, []]])");
  verifyQueryResult(query, expected->slice());
}

TEST_F(SpliceSubqueryNodeOptimizerRuleTest, splice_subquery_skip_nodes) {
  auto query = R"aql(
    FOR k IN 1..10
      LET sub1 = (
        FOR j IN 1..10
        LET sub2 = (
          FOR i IN 1..4
          LIMIT 2,10
          RETURN i
        )
        LIMIT 2,10
        RETURN [j, sub2]
      )
      LIMIT 3, 10
      RETURN [k, sub1])aql";
  verifySubquerySplicing(query, 2);

  VPackBuilder builder;
  builder.openArray();
  for (size_t k = 4; k <= 10; ++k) {
    builder.openArray();
    builder.add(VPackValue(k));
    builder.openArray();
    for (size_t j = 3; j <= 10; ++j) {
      builder.openArray();
      builder.add(VPackValue(j));
      builder.openArray();
      for (size_t i = 3; i <= 4; ++i) {
        builder.add(VPackValue(i));
      }
      builder.close();
      builder.close();
    }
    builder.close();
    builder.close();
  }
  builder.close();
  verifyQueryResult(query, builder.slice());
}

// TODO Check isInSplicedSubquery
// TODO Test cluster rules

}  // namespace aql
}  // namespace tests
}  // namespace arangodb
