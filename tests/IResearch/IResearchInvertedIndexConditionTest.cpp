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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "gtest/gtest.h"

#include "Aql/Ast.h"
#include "Aql/AttributeNamePath.h"
#include "Aql/ExecutionNode/CalculationNode.h"
#include "Aql/ExecutionNode/SortNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Projections.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/StaticStrings.h"
#include "IResearch/AqlHelper.h"
#include "IResearch/ExpressionContextMock.h"
#include "IResearch/IResearchInvertedIndex.h"
#include "Mocks/LogLevels.h"
#include "Mocks/Servers.h"
#include "Mocks/StorageEngineMock.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/FlushFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"

using namespace arangodb::aql;

namespace {
auto createAttributeNamePath = [](std::vector<std::string>&& vec,
                                  arangodb::ResourceMonitor& resMonitor) {
  return AttributeNamePath(std::move(vec), resMonitor);
};
}

class IResearchInvertedIndexConditionTest
    : public ::testing::Test,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 protected:
  arangodb::tests::mocks::MockAqlServer server;

 private:
  TRI_vocbase_t* _vocbase;
  std::shared_ptr<arangodb::LogicalCollection> _collection;

 protected:
  IResearchInvertedIndexConditionTest() : server(false) {
    arangodb::tests::init();
    server.addFeature<arangodb::FlushFeature>(false);
    server.startFeatures();
    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
    dbFeature.createDatabase(testDBInfo(server.server()), _vocbase);

    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *_vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        _collection);
  }

  ~IResearchInvertedIndexConditionTest() override { _collection.reset(); }

  void assertProjectionsCoverageSuccess(
      std::vector<std::vector<std::string>> const& storedFields,
      std::vector<arangodb::aql::AttributeNamePath> const& attributes,
      Projections const& expected) {
    Projections projections(attributes);
    arangodb::IndexId id(1);
    std::vector<std::string> fields = {"a"};
    auto builder = getInvertedIndexPropertiesSlice(id, fields, &storedFields,
                                                   nullptr, "unique_name2");
    bool created = false;
    auto inverted =
        _collection->createIndex(builder.slice(), created).waitAndGet();
    ASSERT_TRUE(created);
    ASSERT_TRUE(inverted);
    auto* index = dynamic_cast<arangodb::iresearch::IResearchInvertedIndex*>(
        inverted.get());
    ASSERT_TRUE(index);
    irs::Finally scope = [&]() noexcept { ASSERT_TRUE(inverted->drop().ok()); };

    ASSERT_TRUE(index->covers(projections));
    ASSERT_EQ(expected.size(), projections.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      ASSERT_EQ(expected[i].path, projections[i].path);
      ASSERT_EQ(expected[i].coveringIndexCutoff,
                projections[i].coveringIndexCutoff);
      ASSERT_EQ(expected[i].coveringIndexPosition,
                projections[i].coveringIndexPosition);
    }
  }

  template<typename Fields>
  void estimateFilterCondition(
      std::string const& queryString, Fields const& fields,
      arangodb::Index::FilterCosts const& expectedCosts,
      ExpressionContext* exprCtx = nullptr,
      std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr,
      std::string const& refName = "d") {
    SCOPED_TRACE(testing::Message("estimateFilterCondition failed for query:<")
                 << queryString
                 << "> Expected support:" << expectedCosts.supportsCondition);
    arangodb::IndexId id(1);
    auto builder = getInvertedIndexPropertiesSlice(id, fields, nullptr, nullptr,
                                                   "unique_name3");
    bool created = false;
    auto inverted =
        _collection->createIndex(builder.slice(), created).waitAndGet();
    ASSERT_TRUE(created);
    ASSERT_TRUE(inverted);
    auto* index = dynamic_cast<arangodb::iresearch::IResearchInvertedIndex*>(
        inverted.get());
    ASSERT_TRUE(index);
    irs::Finally scope = [&]() noexcept { ASSERT_TRUE(inverted->drop().ok()); };

    auto indexFields =
        arangodb::iresearch::IResearchInvertedIndex::fields(index->meta());
    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase(), arangodb::transaction::OperationOriginTestCase{});
    auto query = Query::create(ctx, QueryString(queryString), bindVars);

    ASSERT_NE(query.get(), nullptr);
    auto const parseResult = query->parse();
    ASSERT_TRUE(parseResult.result.ok());

    auto* ast = query->ast();
    ASSERT_TRUE(ast);

    auto* root = ast->root();
    ASSERT_TRUE(root);

    // find first FILTER node
    AstNode* filterNode = nullptr;
    for (size_t i = 0; i < root->numMembers(); ++i) {
      auto* node = root->getMemberUnchecked(i);
      ASSERT_TRUE(node);

      if (NODE_TYPE_FILTER == node->type) {
        filterNode = node;
        break;
      }
    }
    ASSERT_TRUE(filterNode);

    // find referenced variable
    auto* allVars = ast->variables();
    ASSERT_TRUE(allVars);
    Variable* ref = nullptr;
    for (auto entry : allVars->variables(true)) {
      if (entry.second == refName) {
        ref = allVars->getVariable(entry.first);
        break;
      }
    }
    ASSERT_TRUE(ref);

    // optimization time
    {
      // We use this noop transaction because query transaction is empty
      // TODO(MBkkt) Needs ability to create empty transaction
      //  with failed begin but correct in other
      arangodb::transaction::Methods trx{ctx};
      auto costs = index->supportsFilterCondition(trx, id, indexFields, {},
                                                  filterNode, ref, 0);
      ASSERT_EQ(expectedCosts.supportsCondition, costs.supportsCondition);
    }
    // runtime is not intended - we must decide during optimize time!
  }

  void estimateSortCondition(
      std::string const& queryString,
      std::vector<std::pair<std::string, bool>> const& fields,
      arangodb::Index::SortCosts const& expectedCosts,
      ExpressionContext* exprCtx = nullptr,
      std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr,
      std::string const& refName = "d") {
    SCOPED_TRACE(testing::Message("estimateSortCondition failed for query:<")
                 << queryString
                 << "> Expected support:" << expectedCosts.supportsCondition);
    arangodb::IndexId id(1);
    std::vector<std::string> indexFields;
    for (auto const& f : fields) {
      indexFields.push_back(f.first);
    }
    auto builder = getInvertedIndexPropertiesSlice(id, indexFields, nullptr,
                                                   &fields, "unique_name3");
    bool created = false;
    auto inverted =
        _collection->createIndex(builder.slice(), created).waitAndGet();
    ASSERT_TRUE(created);
    ASSERT_TRUE(inverted);
    auto* index = dynamic_cast<arangodb::iresearch::IResearchInvertedIndex*>(
        inverted.get());
    ASSERT_TRUE(index);
    irs::Finally scope = [&]() noexcept { ASSERT_TRUE(inverted->drop().ok()); };

    auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
        vocbase(), arangodb::transaction::OperationOriginTestCase{});
    auto query = Query::create(ctx, QueryString(queryString), bindVars);

    ASSERT_NE(query.get(), nullptr);
    auto aborter = std::make_shared<arangodb::aql::QueryAborter>(query);
    query->prepareQuery(aborter);
    auto* ast = query->ast();
    ASSERT_TRUE(ast);
    auto* root = ast->root();
    ASSERT_TRUE(root);
    auto plan = query->plan();
    ASSERT_TRUE(plan);

    std::vector<std::pair<Variable const*, bool>> sorts;
    {
      arangodb::containers::SmallVector<ExecutionNode*, 8> sortNodes;
      plan->findNodesOfType(sortNodes, {ExecutionNode::SORT}, false);
      for (auto s : sortNodes) {
        auto sortNode = arangodb::aql::ExecutionNode::castTo<SortNode*>(s);

        for (auto& it : sortNode->elements()) {
          sorts.emplace_back(it.var, it.ascending);
        }
      }
    }

    std::unordered_map<VariableId, AstNode const*> variableDefinitions;
    {
      arangodb::containers::SmallVector<ExecutionNode*, 8> setNodes;
      plan->findNodesOfType(setNodes, {ExecutionNode::CALCULATION}, false);
      for (auto n : setNodes) {
        auto en = ExecutionNode::castTo<CalculationNode const*>(n);
        variableDefinitions.try_emplace(en->outVariable()->id,
                                        en->expression()->node());
      }
    }

    // find referenced variable
    auto* allVars = ast->variables();
    ASSERT_TRUE(allVars);
    arangodb::aql::Variable* ref = nullptr;
    for (auto entry : allVars->variables(true)) {
      if (entry.second == refName) {
        ref = allVars->getVariable(entry.first);
        break;
      }
    }
    ASSERT_TRUE(ref);

    std::vector<std::vector<arangodb::basics::AttributeName>> constAttributes;
    arangodb::containers::HashSet<std::vector<arangodb::basics::AttributeName>>
        nonNullAttributes;
    {
      SortCondition sortCondition(query->plan(), sorts, constAttributes,
                                  nonNullAttributes, variableDefinitions);
      auto costs = index->supportsSortCondition(&sortCondition, ref, 0);
      ASSERT_EQ(expectedCosts.supportsCondition, costs.supportsCondition);
      ASSERT_EQ(expectedCosts.coveredAttributes, costs.coveredAttributes);
      ASSERT_EQ(expectedCosts.estimatedCosts, costs.estimatedCosts);
    }
    // runtime is not intended - we must decide during optimize time!
  }

  arangodb::LogicalCollection& collection() { return *_collection; }
  TRI_vocbase_t& vocbase() { return *_vocbase; }
};  // IResearchFilterSetup

TEST_F(IResearchInvertedIndexConditionTest, test_with_equality) {
  std::string queryString = "FOR d IN test FILTER d.a == 'value' RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_not_mix_atr) {
  std::string queryString =
      "FOR c IN test FOR d IN test FILTER d.a == c.missing RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_equality_index) {
  std::string queryString = "FOR d IN test FILTER d.a[5] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_equality_index_attribute) {
  std::string queryString = "FOR d IN test FILTER d['a'] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_equality_index_attribute_chain) {
  std::string queryString =
      "FOR d IN test FILTER d.a['b'] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a.b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_equality_index_attribute_chain_missing) {
  std::string queryString =
      "FOR d IN test FILTER d['a']['c'] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a.b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_equality_index_attribute_missing) {
  std::string queryString = "FOR d IN test FILTER d['a'] == 'value' RETURN d ";
  std::vector<std::string> fields = {"b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_equality_expansion) {
  std::string queryString = "FOR d IN test FILTER d.a[*] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_simple_expression) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' AND (1==1) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_simple_expression_normalization) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' AND (1==d.a) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_equality_many_fields) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' OR d.b == 'value2' AND d.c == "
      "'value3' RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_fcalls) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' OR d.b == 'value2' AND d.c == "
      "UPPER('value3') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_fcalls_on_ref) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' OR d.b == 'value2' AND UPPER(d.c) "
      "== UPPER('value3') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_array_comparison) {
  std::string queryString =
      "FOR d IN test FILTER [1,2,3] ALL IN d.a  RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_array_comparison_ref) {
  std::string queryString =
      "FOR d IN test FILTER ['A', 'B', 'C', UPPER(d.a)] ANY IN d.a  RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_array_as_nodeterm_var_comparison) {
  std::string queryString =
      "LET arr = [1,2, NOOPT(3)] FOR d IN test FILTER arr ALL IN d.a  RETURN "
      "d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition =
      true;  // we can support as NOOPT will be evaluated out of our scope
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_array_as_var_comparison) {
  std::string queryString =
      "LET arr = [1,2, 3] FOR d IN test FILTER arr ALL IN d.a  RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_in_array) {
  std::string queryString =
      "LET arr = [1,2,3] FOR d IN test FILTER d.a IN arr RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_in_nondeterm_array) {
  std::string queryString =
      "LET arr = [1,2,NOOPT(3)] FOR d IN test FILTER d.a IN arr RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition =
      true;  // NOOPT is evaluated out of our loop - so we support this
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_in_nondeterm_array_ref) {
  std::string queryString = "FOR d IN test FILTER d.a IN [1,2, d.c] RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_range) {
  std::string queryString = "FOR d IN test FILTER d.a IN 1..10 RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_nondet_var_range) {
  std::string queryString =
      "LET lim = NOOPT(10) FOR d IN test FILTER d.a IN 1..lim RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_nondet_range) {
  std::string queryString =
      "FOR d IN test FILTER d.a IN 1..NOOPT(10) RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_range_as_var) {
  std::string queryString =
      "LET r = 1..10 FOR d IN test FILTER d.a IN r RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_nondet_range_as_var) {
  std::string queryString =
      "LET r = 1..NOOPT(10) FOR d IN test FILTER d.a IN r RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_negation) {
  std::string queryString = "FOR d IN test FILTER NOT(d.a == 'c') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_nondet_negation) {
  std::string queryString = "FOR d IN test FILTER NOT(d.a == d.b) RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_boost) {
  std::string queryString =
      "FOR d IN test FILTER BOOST(d.a == 10, 10) RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_nondet_boost) {
  std::string queryString =
      "FOR d IN test FILTER BOOST(d.a == d.b, 10) RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_nondet_analyzer) {
  ExpressionContextMock ctx;  // need this for trx for analyzer pool
  std::string queryString =
      "FOR d IN test FILTER ANALYZER(d.a == d.b, 'text_en') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_analyzer) {
  ExpressionContextMock ctx;  // need this for trx for analyzer pool
  std::string queryString =
      "FOR d IN test FILTER ANALYZER(d.a == '10', 'text_en') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_exists) {
  std::string queryString =
      "FOR d IN test FILTER EXISTS(d.a, 'string') RETURN d ";
  std::vector<std::string> fields = {"a", "b", "c", "d"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_no_fields) {
  std::string queryString = "FOR d IN test FILTER d.a == 'value' RETURN d ";
  std::vector<std::string> fields = {"b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_sub_fields_no_nested) {
  std::string queryString = "FOR d IN test FILTER d.b == 'value' RETURN d ";
  std::vector<std::string> fields = {"b.a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_sub_fields_wrong_nested) {
  std::string queryString = "FOR d IN test FILTER d.b.a == 'value' RETURN d ";
  std::vector<std::string> fields = {"b.c"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_sub_fields_covered) {
  std::string queryString = "FOR d IN test FILTER d.b.a == 'value' RETURN d ";
  std::vector<std::string> fields = {"b.a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_no_fields_one_missing) {
  std::string queryString =
      "FOR d IN test FILTER d.a == 'value' OR d.b == 'c' RETURN d ";
  std::vector<std::string> fields = {"b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_nondeterm_expression) {
  std::string queryString =
      "FOR d IN test FILTER d.a == NOOPT('value') RETURN d ";
  std::vector<std::string> fields = {"a"};
  estimateFilterCondition(queryString, fields,
                          arangodb::Index::FilterCosts::defaultCosts(0));
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_subquery_same_atr) {
  std::string queryString =
      "FOR a IN test FOR d IN test FILTER d.a == a.a RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_subquery_not_same_atr) {
  std::string queryString =
      "FOR a IN test FOR d IN test FILTER d.a == a.b RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_subquery_fcall) {
  std::string queryString =
      "FOR a IN test FOR d IN test FILTER d.a == UPPER(a.b) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_subquery_non_determ_fcall) {
  std::string queryString =
      "FOR a IN test2 FOR d IN test FILTER d.a == NOOPT(a.b) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_range_func) {
  std::string queryString =
      "LET a  = 10  FOR d IN test FILTER IN_RANGE(d.a, a, 20, true, true) "
      "RETURN d ";
  ExpressionContextMock ctx;
  auto obj = VPackParser::fromJson("10");
  ctx.vars.emplace("a", arangodb::aql::AqlValue(obj->slice()));
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_range_func_bind) {
  auto obj = VPackParser::fromJson("10");
  ExpressionContextMock ctx;
  ctx.vars.emplace("x", arangodb::aql::AqlValue(obj->slice()));
  auto obj2 = VPackParser::fromJson("20");
  ctx.vars.emplace("a", arangodb::aql::AqlValue(obj2->slice()));
  std::string queryString =
      "LET a  = 20 LET x = 10  FOR d IN test FILTER IN_RANGE(d.a, x, a, true, "
      "true) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_levenshtein_nondet) {
  ExpressionContextMock ctx;
  auto obj2 = VPackParser::fromJson("2");
  ctx.vars.emplace("a", arangodb::aql::AqlValue(obj2->slice()));
  std::string queryString =
      "LET a  = 2 FOR d IN test FILTER LEVENSHTEIN_MATCH(d.a, 'sometext', "
      "NOOPT(a), true, 5) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_levenshtein) {
  ExpressionContextMock ctx;
  auto obj2 = VPackParser::fromJson("2");
  ctx.vars.emplace("a", arangodb::aql::AqlValue(obj2->slice()));
  std::string queryString =
      "LET a  = 2 FOR d IN test FILTER LEVENSHTEIN_MATCH(d.a, 'sometext', a, "
      "true, 5) RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_levenshtein_longdist) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN test FILTER LEVENSHTEIN_MATCH(d.a, 'sometext', 10, true, 5) "
      "RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_object_equal) {
  ExpressionContextMock ctx;
  std::string queryString = "FOR d IN test FILTER  d.a == {a:1, b:2} RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_array_comparison_righthand) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN test FILTER  d.a ANY IN [1,2,3] RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_array_comparison_equality) {
  ExpressionContextMock ctx;
  std::string queryString = "FOR d IN test FILTER [1,2,3] ANY == d.a RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_attribute_covering_one) {
  // simple
  arangodb::GlobalResourceMonitor globalResourceMonitor{};
  arangodb::ResourceMonitor resMonitor{globalResourceMonitor};
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(createAttributeNamePath({"a"}, resMonitor));
    std::vector<std::vector<std::string>> fields = {{"a"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 1;
    expected[0].coveringIndexPosition = 0;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }
  // sub-attribute
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(
        createAttributeNamePath({"a", "b", "c"}, resMonitor));
    std::vector<std::vector<std::string>> fields = {{"a.b.c"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 3;
    expected[0].coveringIndexPosition = 0;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }

  // sub-attribute partial
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(
        createAttributeNamePath({"a", "b", "c"}, resMonitor));
    std::vector<std::vector<std::string>> fields = {{"a.b"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 2;
    expected[0].coveringIndexPosition = 0;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }
}

TEST_F(IResearchInvertedIndexConditionTest, test_attribute_covering_multiple) {
  arangodb::GlobalResourceMonitor globalResourceMonitor{};
  arangodb::ResourceMonitor resMonitor{globalResourceMonitor};
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(createAttributeNamePath({"a"}, resMonitor));
    attributes.emplace_back("c", resMonitor);
    std::vector<std::vector<std::string>> fields = {{"a"}, {"b"}, {"c"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 1;
    expected[0].coveringIndexPosition = 0;
    expected[1].coveringIndexCutoff = 1;
    expected[1].coveringIndexPosition = 2;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }

  // sub-attribute
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(
        createAttributeNamePath({"a", "b", "c"}, resMonitor));
    attributes.emplace_back(createAttributeNamePath({"d"}, resMonitor));
    std::vector<std::vector<std::string>> fields = {{"a.b.c"}, {"d"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 3;
    expected[0].coveringIndexPosition = 0;
    expected[1].coveringIndexCutoff = 1;
    expected[1].coveringIndexPosition = 1;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }

  // sub-attribute partial
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(
        createAttributeNamePath({"a", "b", "c"}, resMonitor));
    attributes.emplace_back("d", resMonitor);
    std::vector<std::vector<std::string>> fields = {{"a.b"}, {"d"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 2;
    expected[0].coveringIndexPosition = 0;
    expected[1].coveringIndexCutoff = 1;
    expected[1].coveringIndexPosition = 1;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }

  // sub-attribute partial more complex
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(createAttributeNamePath({"a", "b"}, resMonitor));
    attributes.emplace_back(createAttributeNamePath({"b", "d"}, resMonitor));
    attributes.emplace_back("d", resMonitor);
    std::vector<std::vector<std::string>> fields = {{"a.b"}, {"b.d"}, {"d"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 2;
    expected[0].coveringIndexPosition = 0;
    expected[1].coveringIndexCutoff = 2;
    expected[1].coveringIndexPosition = 1;
    expected[2].coveringIndexCutoff = 1;
    expected[2].coveringIndexPosition = 2;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }

  // sub-attribute partial - check if the best is selected
  {
    std::vector<arangodb::aql::AttributeNamePath> attributes;
    attributes.emplace_back(createAttributeNamePath({"a", "b"}, resMonitor));
    attributes.emplace_back(createAttributeNamePath({"b", "d"}, resMonitor));
    attributes.emplace_back(createAttributeNamePath({"d"}, resMonitor));
    std::vector<std::vector<std::string>> fields = {
        {"a.b"}, {"b.d"}, {"a.b", "b.d", "a.c"}, {"d"}};
    arangodb::aql::Projections expected(attributes);
    expected[0].coveringIndexCutoff = 2;
    expected[0].coveringIndexPosition = 2;
    expected[1].coveringIndexCutoff = 2;
    expected[1].coveringIndexPosition = 3;
    expected[2].coveringIndexCutoff = 1;
    expected[2].coveringIndexPosition = 5;
    assertProjectionsCoverageSuccess(fields, attributes, expected);
  }
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::zeroCosts(2);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_subset) {
  ExpressionContextMock ctx;
  std::string queryString = "FOR d IN " + collection().name() +
                            " FILTER  d.a == {a:1, b:2} SORT d.a ASC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::zeroCosts(1);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_invalid_direct) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true}, {"b", true}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_invalid_direct2) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", false},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_invalid_field) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"c", true},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_invalid_field2) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true},
                                                      {"c", false}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_invalid_order) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.b DESC, d.a ASC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, sort_support_not_all) {
  ExpressionContextMock ctx;
  std::string queryString =
      "FOR d IN " + collection().name() +
      " FILTER  d.a == {a:1, b:2} SORT d.a ASC, d.b DESC, d.c ASC RETURN d ";
  std::vector<std::pair<std::string, bool>> fields = {{"a", true},
                                                      {"b", false}};
  auto expected = arangodb::Index::SortCosts::defaultCosts(0);
  estimateSortCondition(queryString, fields, expected, &ctx);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion) {
  std::string queryString = "FOR d IN test FILTER d.a == 'value' RETURN d ";
  std::vector<std::string> fields = {"a[*]"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion_array_op) {
  std::string queryString =
      "FOR d IN test FILTER ['a', 'b'] ALL IN d.a RETURN d ";
  std::vector<std::string> fields = {"a[*]"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion_explicit) {
  std::string queryString = "FOR d IN test FILTER d.a[*] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a[*]"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_expansion_non_array_explicit) {
  std::string queryString = "FOR d IN test FILTER d.a[*] == 'value' RETURN d ";
  std::vector<std::string> fields = {"a"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_expansion_array_op_explicit) {
  std::string queryString =
      "FOR d IN test FILTER ['a', 'b'] ALL IN d.a[*] RETURN d ";
  std::vector<std::string> fields = {"a[*]"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion_sub) {
  std::string queryString =
      "FOR d IN test FILTER d.a[*].b == 'value' RETURN d ";
  std::vector<std::string> fields = {"a[*].b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion_array_op_sub) {
  std::string queryString =
      "FOR d IN test FILTER ['a', 'b'] ALL IN d.a[*].b RETURN d ";
  std::vector<std::string> fields = {"a[*].b"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  expected.supportsCondition = true;
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest, test_with_expansion_sub_miss) {
  std::string queryString = "FOR d IN test FILTER d.a.b == 'value' RETURN d ";
  std::vector<std::string> fields = {"a[*].missing"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_expansion_array_op_sub_miss) {
  std::string queryString =
      "FOR d IN test FILTER ['a', 'b'] ALL IN d.a.b RETURN d ";
  std::vector<std::string> fields = {"a[*].missing"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_expansion_sub_miss_explicit) {
  std::string queryString =
      "FOR d IN test FILTER d.a[*].b == 'value' RETURN d ";
  std::vector<std::string> fields = {"a[*].missing"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

TEST_F(IResearchInvertedIndexConditionTest,
       test_with_expansion_array_op_sub_miss_explicit) {
  std::string queryString =
      "FOR d IN test FILTER ['a', 'b'] ALL IN d.a[*].b RETURN d ";
  std::vector<std::string> fields = {"a[*].missing"};
  auto expected = arangodb::Index::FilterCosts::defaultCosts(0);
  estimateFilterCondition(queryString, fields, expected);
}

#if USE_ENTERPRISE
#include "tests/IResearch/IResearchInvertedIndexConditionTestEE.h"
#endif
