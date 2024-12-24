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

#include <absl/strings/str_replace.h>

#include <velocypack/Iterator.h>

#include "Aql/AqlFunctionFeature.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/OptimizerRule.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Async/async.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchLink.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchView.h"
#include "IResearchQueryCommon.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

#include "search/boolean_filter.hpp"
#include "search/levenshtein_filter.hpp"
#include "search/prefix_filter.hpp"
#include "search/range_filter.hpp"
#include "search/term_filter.hpp"

namespace arangodb::aql {
class ExecutionNode;
}

namespace arangodb::tests {
namespace {

class QueryTestMulti
    : public ::testing::TestWithParam<
          std::tuple<arangodb::ViewType, arangodb::iresearch::LinkVersion>>,
      public arangodb::tests::LogSuppressor<arangodb::Logger::AUTHENTICATION,
                                            arangodb::LogLevel::ERR> {
 private:
  TRI_vocbase_t* _vocbase{nullptr};

 protected:
  arangodb::tests::mocks::MockAqlServer server;

  virtual arangodb::ViewType type() const { return std::get<0>(GetParam()); }

  QueryTestMulti() : server{false} {
    arangodb::tests::init(true);

    server.addFeature<arangodb::FlushFeature>(false);
    server.startFeatures();

    auto& analyzers =
        server.getFeature<arangodb::iresearch::IResearchAnalyzerFeature>();
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;

    auto& dbFeature = server.getFeature<arangodb::DatabaseFeature>();
    // required for IResearchAnalyzerFeature::emplace(...)
    dbFeature.createDatabase(testDBInfo(server.server()), _vocbase);

    std::shared_ptr<arangodb::LogicalCollection> unused;
    arangodb::OperationOptions options(arangodb::ExecContext::current());
    arangodb::methods::Collections::createSystem(
        *_vocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
    unused = nullptr;

    auto res = analyzers.emplace(
        result, "testVocbase::test_analyzer", "TestAnalyzer",
        VPackParser::fromJson("\"abc\"")->slice(),
        arangodb::transaction::OperationOriginTestCase{},
        arangodb::iresearch::Features(
            {}, irs::IndexFeatures::FREQ |
                    irs::IndexFeatures::POS));  // required for PHRASE
    EXPECT_TRUE(res.ok());

    res = analyzers.emplace(
        result, "testVocbase::test_csv_analyzer", "TestDelimAnalyzer",
        VPackParser::fromJson("\",\"")->slice(),
        arangodb::transaction::OperationOriginTestCase{});  // cache analyzer
    EXPECT_TRUE(res.ok());

    res = analyzers.emplace(
        result, "testVocbase::text_en", "text",
        VPackParser::fromJson(
            "{ \"locale\": \"en.UTF-8\", \"stopwords\": [ ] }")
            ->slice(),
        arangodb::transaction::OperationOriginTestCase{},
        arangodb::iresearch::Features{
            arangodb::iresearch::FieldFeatures::NORM,
            irs::IndexFeatures::FREQ |
                irs::IndexFeatures::POS});  // cache analyzer
    EXPECT_TRUE(res.ok());

    auto sysVocbase =
        server.getFeature<arangodb::SystemDatabaseFeature>().use();
    arangodb::methods::Collections::createSystem(
        *sysVocbase, options, arangodb::tests::AnalyzerCollectionName, false,
        unused);
    unused = nullptr;

    res =
        analyzers.emplace(result, "_system::test_analyzer", "TestAnalyzer",
                          VPackParser::fromJson("\"abc\"")->slice(),
                          arangodb::transaction::OperationOriginTestCase{},
                          arangodb::iresearch::Features{
                              irs::IndexFeatures::FREQ |
                              irs::IndexFeatures::POS});  // required for PHRASE

    res = analyzers.emplace(
        result, "_system::ngram_test_analyzer13", "ngram",
        VPackParser::fromJson("{\"min\":1, \"max\":3, \"streamType\":\"utf8\", "
                              "\"preserveOriginal\":false}")
            ->slice(),
        arangodb::transaction::OperationOriginTestCase{},
        arangodb::iresearch::Features{
            irs::IndexFeatures::FREQ |
            irs::IndexFeatures::POS});  // required for PHRASE

    res = analyzers.emplace(
        result, "_system::ngram_test_analyzer2", "ngram",
        VPackParser::fromJson("{\"min\":2, \"max\":2, \"streamType\":\"utf8\", "
                              "\"preserveOriginal\":false}")
            ->slice(),
        arangodb::transaction::OperationOriginTestCase{},
        arangodb::iresearch::Features{
            irs::IndexFeatures::FREQ |
            irs::IndexFeatures::POS});  // required for PHRASE

    EXPECT_TRUE(res.ok());

    res = analyzers.emplace(
        result, "_system::test_csv_analyzer", "TestDelimAnalyzer",
        VPackParser::fromJson("\",\"")->slice(),
        arangodb::transaction::OperationOriginTestCase{});  // cache analyzer
    EXPECT_TRUE(res.ok());

    auto& functions = server.getFeature<arangodb::aql::AqlFunctionFeature>();
    // register fake non-deterministic function in order to suppress
    // optimizations
    functions.add(arangodb::aql::Function{
        "_NONDETERM_", ".",
        arangodb::aql::Function::makeFlags(
            // fake non-deterministic
            arangodb::aql::Function::Flags::CanRunOnDBServerCluster,
            arangodb::aql::Function::Flags::CanRunOnDBServerOneShard),
        [](arangodb::aql::ExpressionContext*, arangodb::aql::AstNode const&,
           arangodb::aql::functions::VPackFunctionParametersView params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    // register fake non-deterministic function in order to suppress
    // optimizations
    functions.add(arangodb::aql::Function{
        "_FORWARD_", ".",
        arangodb::aql::Function::makeFlags(
            // fake deterministic
            arangodb::aql::Function::Flags::Deterministic,
            arangodb::aql::Function::Flags::Cacheable,
            arangodb::aql::Function::Flags::CanRunOnDBServerCluster,
            arangodb::aql::Function::Flags::CanRunOnDBServerOneShard),
        [](arangodb::aql::ExpressionContext*, arangodb::aql::AstNode const&,
           arangodb::aql::functions::VPackFunctionParametersView params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    // external function names must be registred in upper-case
    // user defined functions have ':' in the external function name
    // function arguments string format:
    // requiredArg1[,requiredArg2]...[|optionalArg1[,optionalArg2]...]
    arangodb::aql::Function customScorer(
        "CUSTOMSCORER", ".|+",
        arangodb::aql::Function::makeFlags(
            arangodb::aql::Function::Flags::Deterministic,
            arangodb::aql::Function::Flags::Cacheable,
            arangodb::aql::Function::Flags::CanRunOnDBServerCluster,
            arangodb::aql::Function::Flags::CanRunOnDBServerOneShard),
        nullptr);
    arangodb::iresearch::addFunction(functions, customScorer);

    auto& dbPathFeature = server.getFeature<arangodb::DatabasePathFeature>();
    arangodb::tests::setDatabasePath(
        dbPathFeature);  // ensure test data is stored in a unique directory
  }

  TRI_vocbase_t& vocbase() {
    TRI_ASSERT(_vocbase != nullptr);
    return *_vocbase;
  }

  arangodb::iresearch::LinkVersion linkVersion() const noexcept {
    return std::get<1>(GetParam());
  }

  arangodb::iresearch::LinkVersion version() const noexcept {
    return std::get<1>(GetParam());
  }
};

static const VPackBuilder systemDatabaseBuilder = dbArgsBuilder();
static const VPackSlice systemDatabaseArgs = systemDatabaseBuilder.slice();

bool findEmptyNodes(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr) {
  auto options = VPackParser::fromJson(
      //    "{ \"tracing\" : 1 }"
      "{ }");

  auto query = arangodb::aql::Query::create(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(options->slice()));

  waitForAsync(query->prepareQuery());

  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;

  // try to find `EnumerateViewNode`s and process corresponding filters and
  // sorts
  query->plan()->findNodesOfType(nodes, arangodb::aql::ExecutionNode::NORESULTS,
                                 true);
  return !nodes.empty();
}

class QueryOptimization : public QueryTestMulti {
 protected:
  std::deque<std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>>>
      insertedDocs;

  void addLinkToCollection(
      std::shared_ptr<arangodb::iresearch::IResearchView>& view) {
    auto versionStr = std::to_string(static_cast<uint32_t>(linkVersion()));

    auto updateJson = VPackParser::fromJson(
        "{ \"links\" : {"
        "\"collection_1\" : { \"includeAllFields\" : true, \"version\": " +
        versionStr +
        " }"
        "}}");
    EXPECT_TRUE(view->properties(updateJson->slice(), true, true).ok());

    arangodb::velocypack::Builder builder;

    builder.openObject();
    auto res = view->properties(
        builder, arangodb::LogicalDataSource::Serialization::Properties);
    ASSERT_TRUE(res.ok());
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_EQ(slice.get("name").copyString(), "testView");
    EXPECT_TRUE(slice.get("type").copyString() ==
                arangodb::iresearch::StaticStrings::ViewArangoSearchType);
    EXPECT_TRUE(slice.get("deleted").isNone());  // no system properties
    auto tmpSlice = slice.get("links");
    EXPECT_TRUE(tmpSlice.isObject() && 1 == tmpSlice.length());
  }

  void SetUp() override {
    std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;

    // add collection_1
    {
      auto collectionJson =
          VPackParser::fromJson("{ \"name\": \"collection_1\" }");
      logicalCollection1 = vocbase().createCollection(collectionJson->slice());
      ASSERT_NE(nullptr, logicalCollection1);
    }

    // add view
    if (type() == ViewType::kArangoSearch) {
      auto createJson = VPackParser::fromJson(
          "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
      auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
          vocbase().createView(createJson->slice(), false));
      ASSERT_FALSE(!view);

      // add link to collection
      addLinkToCollection(view);
    } else {
      auto createJson = VPackParser::fromJson(
          "{ \"name\": \"testView\", \"type\": \"search-alias\" }");
      auto view = std::dynamic_pointer_cast<arangodb::iresearch::Search>(
          vocbase().createView(createJson->slice(), false));
      ASSERT_FALSE(!view);
      bool created = false;
      createJson = VPackParser::fromJson(absl::Substitute(
          R"({ "name": "index_1", "type": "inverted",
               "version": $0,
               "includeAllFields": true })",
          version()));
      logicalCollection1->createIndex(createJson->slice(), created)
          .waitAndGet();
      ASSERT_TRUE(created);
      auto const viewDefinition = absl::Substitute(R"({ "indexes": [
        { "collection": "collection_1", "index": "index_1"}
      ]})");
      auto updateJson = arangodb::velocypack::Parser::fromJson(viewDefinition);
      auto r = view->properties(updateJson->slice(), true, true);
      EXPECT_TRUE(r.ok()) << r.errorMessage();
    }

    // populate view with the data
    {
      arangodb::OperationOptions opt;
      static std::vector<std::string> const EMPTY;
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::create(
              vocbase(), arangodb::transaction::OperationOriginTestCase{}),
          EMPTY, {logicalCollection1->name()}, EMPTY,
          arangodb::transaction::Options());
      EXPECT_TRUE(trx.begin().ok());

      // insert into collection
      auto builder =
          VPackParser::fromJson("[{ \"values\" : [ \"A\", \"C\", \"B\" ] }]");

      auto root = builder->slice();
      ASSERT_TRUE(root.isArray());

      for (auto doc : arangodb::velocypack::ArrayIterator(root)) {
        auto res = trx.insert(logicalCollection1->name(), doc, opt);
        EXPECT_TRUE(res.ok());

        res = trx.document(logicalCollection1->name(), res.slice(), opt);
        EXPECT_TRUE(res.ok());
        insertedDocs.emplace_back(std::move(res.buffer));
      }

      EXPECT_TRUE(trx.commit().ok());
      EXPECT_TRUE((arangodb::tests::executeQuery(
                       vocbase(),
                       "FOR d IN testView SEARCH 1 ==1 OPTIONS "
                       "{ waitForSync: true } RETURN d")
                       .result.ok()));  // commit
    }
  }
};

std::vector<std::string> optimizerOptionsAvailable = {
    "", " OPTIONS {\"conditionOptimization\":\"auto\"} ",
    " OPTIONS {\"conditionOptimization\":\"nodnf\"} ",
    " OPTIONS {\"conditionOptimization\":\"noneg\"} ",
    " OPTIONS {\"conditionOptimization\":\"none\"} "};

constexpr size_t disabledDnfOptimizationStart = 2;

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

// dedicated to https://github.com/arangodb/arangodb/issues/8294
// a IN [ x ] && a == y, x < y
TEST_P(QueryOptimization, test_1) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values == 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // for all optimization modes query should be the same
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x == y
TEST_P(QueryOptimization, test_2) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'B', 'A' ] AND d.values "
                                  "== 'A'") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x > y
TEST_P(QueryOptimization, test_3) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'B' ] AND d.values == 'A' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x < y
TEST_P(QueryOptimization, test_4) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values != 'D' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x < y
TEST_P(QueryOptimization, test_5) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values != 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x > y
TEST_P(QueryOptimization, test_6) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'D' ] AND d.values != 'D' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

/* FIXME
//  a IN [ x ] && a == y, x == y
TEST_P(QueryOptimization, test_7) {
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'A', 'A' ] AND d.values != 'A'
RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(
      vocbase(), query,
      {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

  std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_EQ(expectedDocs.size(), resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_EQ(0,
              arangodb::basics::VelocyPackHelper::compare(
                  arangodb::velocypack::Slice(*expectedDoc), resolved, true));
  }
  EXPECT_EQ(expectedDoc, expectedDocs.end());
}
*/

// a IN [ x ] && a != y, x > y
TEST_P(QueryOptimization, test_8) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'B' ] AND d.values != 'A' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a != y, x > y
TEST_P(QueryOptimization, test_9) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'B' ] AND d.values != '@' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("@"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a < y, x < y
TEST_P(QueryOptimization, test_10) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'A', "
                                  "'B' ] AND d.values < 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a < y, x == y
TEST_P(QueryOptimization, test_11) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'A', "
                                  "'C' ] AND d.values < 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a < y, x > y
TEST_P(QueryOptimization, test_12) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'D', "
                                  "'C' ] AND d.values < 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a <= y, x < y
TEST_P(QueryOptimization, test_13) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B', "
                                  "'C' ] AND d.values <= 'D' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a <= y, x == y
TEST_P(QueryOptimization, test_14) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B', "
                                  "'C' ] AND d.values <= 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a <= y, x > y
TEST_P(QueryOptimization, test_15) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B', "
                                  "'C' ] AND d.values <= 'A' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a >= y, x < y
TEST_P(QueryOptimization, test_16) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values >= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a >= y, x == y
TEST_P(QueryOptimization, test_17) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values >= 'A' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a >= y, x > y
TEST_P(QueryOptimization, test_18) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'D' ] AND d.values >= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a > y, x < y
TEST_P(QueryOptimization, test_19) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ '@', "
                                  "'A' ] AND d.values > 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a > y, x == y
TEST_P(QueryOptimization, test_20) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'B' ] AND d.values > 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a > y, x > y
TEST_P(QueryOptimization, test_21) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C', "
                                  "'D' ] AND d.values > 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a IN [ y ]
TEST_P(QueryOptimization, test_22) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'A', "
                                  "'B' ] AND d.values IN [ "
                                  "'A', 'B', 'C' ]") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // FIXME optimize
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      {
        auto& sub = root.add<irs::Or>();
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = sub.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x < y
TEST_P(QueryOptimization, test_23) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B' "
                                  "] AND d.values == 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a == y, x == y
TEST_P(QueryOptimization, test_24) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values == 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a == y, x > y
TEST_P(QueryOptimization, test_25) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values == 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a != y, x < y
TEST_P(QueryOptimization, test_26) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'A' "
                                  "] AND d.values != 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [ x ] && a != y, x == y
TEST_P(QueryOptimization, test_27) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values != 'C' ") +
                              o + "RETURN d";

    if (optimizeType < disabledDnfOptimizationStart) {
      EXPECT_TRUE(findEmptyNodes(vocbase(), query));
    } else {
      // no optimization will give us redundant nodes, but that is expected
      EXPECT_TRUE(arangodb::tests::assertRules(
          vocbase(), query,
          {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));
      EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    }

    // check structure
    {
      if (optimizeType < disabledDnfOptimizationStart) {
        irs::And expected;
        auto& root = expected;
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        assertFilterOptimized(vocbase(), query, expected);
      } else {
        irs::Or expected;
        auto& root = expected.add<irs::And>();
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }

        assertFilterOptimized(vocbase(), query, expected);
      }
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [ x ] && a != y, x > y
TEST_P(QueryOptimization, test_28) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values IN ['B'] AND d.values != 'C'") +
        o + " RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a IN [ x ] && a < y, x < y
TEST_P(QueryOptimization, test_29) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B' "
                                  "] AND d.values < 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [ x ] && a < y, x == y
TEST_P(QueryOptimization, test_30) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values < 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a < y, x > y
TEST_P(QueryOptimization, test_31) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values < 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a IN [ x ] && a <= y, x < y
TEST_P(QueryOptimization, test_32) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B' "
                                  "] AND d.values <= 'C' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [x] && a <= y, x == y
TEST_P(QueryOptimization, test_33) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B' "
                                  "] AND d.values <= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [ x ] && a <= y, x > y
TEST_P(QueryOptimization, test_34) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'C' "
                                  "] AND d.values <= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a IN [ x ] && a >= y, x < y
TEST_P(QueryOptimization, test_35) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'A' "
                                  "] AND d.values >= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [ x ] && a >= y, x == y
TEST_P(QueryOptimization, test_36) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH d.values IN [ 'B' "
                                  "] AND d.values >= 'B' ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a IN [x] && a >= y, x > y
TEST_P(QueryOptimization, test_37) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values IN ['C'] AND d.values >= 'B'") +
        o + " RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a IN [x] && a > y, x < y
TEST_P(QueryOptimization, test_38) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values IN ['A'] AND d.values > 'B'") +
        o + " RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a IN [x] && a > y, x == y
TEST_P(QueryOptimization, test_39) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values IN ['B'] AND d.values > 'B'") +
        o + " RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a IN [x] && a > y, x > y
TEST_P(QueryOptimization, test_40) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values IN ['C'] AND d.values > 'B'") +
        o + " RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a == y, x < y
TEST_P(QueryOptimization, test_41) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values == 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a == y, x == y
TEST_P(QueryOptimization, test_42) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a == y, x > y
TEST_P(QueryOptimization, test_43) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a != y, x < y
TEST_P(QueryOptimization, test_44) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'A' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a != y, x == y
TEST_P(QueryOptimization, test_45) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values != 'C'") +
        o + "RETURN d";
    if (optimizeType < disabledDnfOptimizationStart) {
      // FIXME EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
      //  { arangodb::aql::OptimizerRule::handleArangoSearchViewsRule }));
      EXPECT_TRUE(findEmptyNodes(vocbase(), query));
    } else {
      EXPECT_TRUE(arangodb::tests::assertRules(
          vocbase(), query,
          {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));
      EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    }
    // check structure
    {
      if (optimizeType < disabledDnfOptimizationStart) {
        irs::And expected;
        auto& root = expected;
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }

        assertFilterOptimized(vocbase(), query, expected);
      } else {
        irs::Or expected;
        auto& root = expected.add<irs::And>();
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }

        assertFilterOptimized(vocbase(), query, expected);
      }
    }
    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    optimizeType++;
  }
}

// a == x && a != y, x > y
TEST_P(QueryOptimization, test_46) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values != 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a < y, x < y
TEST_P(QueryOptimization, test_47) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values < 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a < y, x == y
TEST_P(QueryOptimization, test_48) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a < y, x > y
TEST_P(QueryOptimization, test_49) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a <= y, x < y
TEST_P(QueryOptimization, test_50) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a <= y, x == y
TEST_P(QueryOptimization, test_51) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a <= y, x > y
TEST_P(QueryOptimization, test_52) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a == x && a >= y, x < y
TEST_P(QueryOptimization, test_53) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'A' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a >= y, x == y
TEST_P(QueryOptimization, test_54) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a >= y, x > y
TEST_P(QueryOptimization, test_55) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a > y, x < y
TEST_P(QueryOptimization, test_56) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'A' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a == x && a > y, x == y
TEST_P(QueryOptimization, test_57) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'B' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      } else {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a == x && a > y, x > y
TEST_P(QueryOptimization, test_58) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values == 'C' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a != x && a == y, x < y
TEST_P(QueryOptimization, test_59) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '@' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("@"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a == y, x < y
TEST_P(QueryOptimization, test_60) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a == y, x == y
TEST_P(QueryOptimization, test_61) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'A'") +
        o + "RETURN d";
    if (optimizeType < disabledDnfOptimizationStart) {
      // FIXME EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
      //  { arangodb::aql::OptimizerRule::handleArangoSearchViewsRule }));
      EXPECT_TRUE(findEmptyNodes(vocbase(), query));
    } else {
      EXPECT_TRUE(arangodb::tests::assertRules(
          vocbase(), query,
          {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

      EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    }
    // check structure
    {
      if (optimizeType < disabledDnfOptimizationStart) {
        irs::And expected;
        auto& root = expected;
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }

        assertFilterOptimized(vocbase(), query, expected);
      } else {
        irs::Or expected;
        auto& root = expected.add<irs::And>();
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }

        assertFilterOptimized(vocbase(), query, expected);
      }
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a != x && a == y, x > y
TEST_P(QueryOptimization, test_62) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a == y, x > y
TEST_P(QueryOptimization, test_63) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'B' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a != y, x < y
TEST_P(QueryOptimization, test_64) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '@' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("@"));
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a != y, x < y
TEST_P(QueryOptimization, test_65) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a != x && a != y, x == y
TEST_P(QueryOptimization, test_66) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a != y, x == y
TEST_P(QueryOptimization, test_67) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a != y, x > y
TEST_P(QueryOptimization, test_68) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'B' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a != x && a < y, x < y
TEST_P(QueryOptimization, test_69) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values < 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a < y, x == y
TEST_P(QueryOptimization, test_70) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a < y, x == y
TEST_P(QueryOptimization, test_71) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '@' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("@"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a < y, x == y
TEST_P(QueryOptimization, test_72) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a < y, x > y
TEST_P(QueryOptimization, test_73) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a < y, x > y
TEST_P(QueryOptimization, test_74) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'C' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a <= y, x < y
TEST_P(QueryOptimization, test_75) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a != x && a <= y, x < y
TEST_P(QueryOptimization, test_76) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a <= y, x == y
TEST_P(QueryOptimization, test_77) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a <= y, x == y
TEST_P(QueryOptimization, test_78) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'B' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a <= y, x > y
TEST_P(QueryOptimization, test_79) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a <= y, x > y
TEST_P(QueryOptimization, test_80) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'C' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x < y
TEST_P(QueryOptimization, test_81) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x < y
TEST_P(QueryOptimization, test_82) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x == y
TEST_P(QueryOptimization, test_83) {
  for (auto& o : optimizerOptionsAvailable) {
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values >= '0'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    SCOPED_TRACE(o);
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x == y
TEST_P(QueryOptimization, test_84) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x > y
TEST_P(QueryOptimization, test_85) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a >= y, x > y
TEST_P(QueryOptimization, test_86) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'C' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x < y
TEST_P(QueryOptimization, test_87) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x < y
TEST_P(QueryOptimization, test_88) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x == y
TEST_P(QueryOptimization, test_89) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != '0' AND d.values > '0'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("0"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x == y
TEST_P(QueryOptimization, test_90) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x > y
TEST_P(QueryOptimization, test_91) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'D' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("D"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a != x && a > y, x > y
TEST_P(QueryOptimization, test_92) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values != 'C' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::Not>().filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a < x && a == y, x < y
TEST_P(QueryOptimization, test_93) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }

      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    optimizeType++;
  }
}

// a < x && a == y, x == y
TEST_P(QueryOptimization, test_94) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a == y, x > y
TEST_P(QueryOptimization, test_95) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }

      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x < y
TEST_P(QueryOptimization, test_96) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x < y
TEST_P(QueryOptimization, test_97) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x == y
TEST_P(QueryOptimization, test_98) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'D' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x == y
TEST_P(QueryOptimization, test_99) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x > y
TEST_P(QueryOptimization, test_100) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values != '0'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("0"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }

      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("0"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a != y, x > y
TEST_P(QueryOptimization, test_101) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a < y, x < y
TEST_P(QueryOptimization, test_102) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a < y, x == y
TEST_P(QueryOptimization, test_103) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a < x && a < y, x > y
TEST_P(QueryOptimization, test_104) {
  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0]->data()),
  };
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a < x && a <= y, x < y
TEST_P(QueryOptimization, test_105) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values <= 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a <= y, x == y
TEST_P(QueryOptimization, test_106) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a <= y, x > y
TEST_P(QueryOptimization, test_107) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a >= y, x < y
TEST_P(QueryOptimization, test_108) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a >= y, x == y
TEST_P(QueryOptimization, test_109) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a >= y, x > y
TEST_P(QueryOptimization, test_110) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a > y, x < y
TEST_P(QueryOptimization, test_111) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a > y, x == y
TEST_P(QueryOptimization, test_112) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a < x && a > y, x > y
TEST_P(QueryOptimization, test_113) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values < 'C' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a == y, x < y
TEST_P(QueryOptimization, test_114) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a == y, x == y
TEST_P(QueryOptimization, test_115) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a == y, x > y
TEST_P(QueryOptimization, test_116) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'B' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x < y
TEST_P(QueryOptimization, test_117) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x < y
TEST_P(QueryOptimization, test_118) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x == y
TEST_P(QueryOptimization, test_119) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'B' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x == y
TEST_P(QueryOptimization, test_120) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'D' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x > y
TEST_P(QueryOptimization, test_121) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values != '@'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a != y, x > y
TEST_P(QueryOptimization, test_122) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a < y, x < y
TEST_P(QueryOptimization, test_123) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a < y, x == y
TEST_P(QueryOptimization, test_124) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'B' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a <= x && a < y, x > y
TEST_P(QueryOptimization, test_125) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a <= y, x < y
TEST_P(QueryOptimization, test_126) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a <= y, x == y
TEST_P(QueryOptimization, test_127) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'B' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a <= x && a <= y, x > y
TEST_P(QueryOptimization, test_128) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a >= y, x < y
TEST_P(QueryOptimization, test_129) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a >= y, x == y
TEST_P(QueryOptimization, test_130) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a >= y, x > y
TEST_P(QueryOptimization, test_131) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a > y, x < y
TEST_P(QueryOptimization, test_132) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a > y, x == y
TEST_P(QueryOptimization, test_133) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a <= x && a > y, x > y
TEST_P(QueryOptimization, test_134) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values <= 'C' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.max =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
          filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a == y, x < y
TEST_P(QueryOptimization, test_135) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a >= x && a == y, x == y
TEST_P(QueryOptimization, test_136) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a == y, x > y
TEST_P(QueryOptimization, test_137) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a >= x && a != y, x < y
TEST_P(QueryOptimization, test_138) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a != y, x < y
TEST_P(QueryOptimization, test_139) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a != y, x == y
TEST_P(QueryOptimization, test_140) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= '@' AND d.values != '@'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a != y, x == y
TEST_P(QueryOptimization, test_141) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}
// a >= x && a != y, x > y
TEST_P(QueryOptimization, test_142) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a != y, x > y
TEST_P(QueryOptimization, test_143) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a < y, x < y
TEST_P(QueryOptimization, test_144) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a < y, x == y
TEST_P(QueryOptimization, test_145) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a < y, x > y
TEST_P(QueryOptimization, test_146) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'C' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a <= y, x < y
TEST_P(QueryOptimization, test_147) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a <= y, x == y
TEST_P(QueryOptimization, test_148) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}
// a >= x && a <= y, x > y
TEST_P(QueryOptimization, test_149) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'C' AND d.values <= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a >= y, x < y
TEST_P(QueryOptimization, test_150) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a >= y, x == y
TEST_P(QueryOptimization, test_151) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a >= x && a >= y, x > y
TEST_P(QueryOptimization, test_152) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'C' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a > y, x < y
TEST_P(QueryOptimization, test_153) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'A' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a > y, x == y
TEST_P(QueryOptimization, test_154) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a >= x && a > y, x > y
TEST_P(QueryOptimization, test_155) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a == y, x < y
TEST_P(QueryOptimization, test_156) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a == y, x == y
TEST_P(QueryOptimization, test_157) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a == y, x > y
TEST_P(QueryOptimization, test_158) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a != y, x < y
TEST_P(QueryOptimization, test_159) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'D'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("D"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a != y, x < y
TEST_P(QueryOptimization, test_160) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a != y, x == y
TEST_P(QueryOptimization, test_161) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > '@' AND d.values != '@'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a != y, x == y
TEST_P(QueryOptimization, test_162) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    optimizeType++;
  }
}

// a > x && a != y, x > y
TEST_P(QueryOptimization, test_163) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values != '@'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("@"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a != y, x > y
TEST_P(QueryOptimization, test_164) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values != 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType < disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      } else {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
        {
          auto& filter = root.add<irs::Not>().filter<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a < y, x < y
TEST_P(QueryOptimization, test_165) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values < 'C'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a < y, x == y
TEST_P(QueryOptimization, test_166) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values < 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));
    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a < y, x > y
TEST_P(QueryOptimization, test_167) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'C' AND d.values < 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a <= y, x < y
TEST_P(QueryOptimization, test_168) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a <= y, x == y
TEST_P(QueryOptimization, test_169) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a <= y, x > y
TEST_P(QueryOptimization, test_170) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.max =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
        filter.mutable_options()->range.max_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a >= y, x < y
TEST_P(QueryOptimization, test_171) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a >= y, x == y
TEST_P(QueryOptimization, test_172) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a >= y, x > y
TEST_P(QueryOptimization, test_173) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::INCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a > y, x < y
TEST_P(QueryOptimization, test_174) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'A' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// a > x && a > y, x == y
TEST_P(QueryOptimization, test_175) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'B'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// a > x && a > y, x > y
TEST_P(QueryOptimization, test_176) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'A'") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_range>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->range.min =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
        filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
      }
      if (optimizeType >= disabledDnfOptimizationStart) {
        {
          auto& filter = root.add<irs::by_range>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->range.min =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
          filter.mutable_options()->range.min_type = irs::BoundType::EXCLUSIVE;
        }
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// check double negation is always collapsed
TEST_P(QueryOptimization, test_177) {
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);

    std::string const query =
        std::string("FOR d IN testView SEARCH  NOT( NOT (d.values == 'B'))") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& filter = root.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
  }
}

// check DNF conversion disabled
TEST_P(QueryOptimization, test_178) {
  auto dnfConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    // left part B && C
    {
      auto& andFilter = root.add<irs::And>();
      {
        auto& filter = andFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = andFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    // right part B && A
    {
      auto& andFilter = root.add<irs::And>();
      {
        auto& filter = andFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = andFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    auto& root = expected.add<irs::And>();
    {
      auto& filter = root.add<irs::by_term>();
      *filter.mutable_field() = mangleStringIdentity("values");
      filter.mutable_options()->term =
          irs::ViewCast<irs::byte_type>(std::string_view("B"));
    }

    {
      auto& sub = root.add<irs::Or>();
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      dnfConvertedExpected, dnfConvertedExpected, nonConvertedExpected,
      nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  d.values == 'B' AND  ( d.values == 'C'  "
            "OR d.values == 'A' ) ") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check DNF conversion disabled  but IN nodes processed (sorted and
// deduplicated)!
TEST_P(QueryOptimization, test_179) {
  auto dnfConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    {
      auto& andFilter = root.add<irs::And>();
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
    }
    {
      auto& andFilter = root.add<irs::And>();
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    {
      auto& filter = root.add<irs::Or>();
      {
        auto& or2 = filter.add<irs::Or>();
        {
          auto& filter = or2.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = or2.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& or2 = filter.add<irs::Or>();
        {
          auto& filter = or2.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = or2.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      dnfConvertedExpected, dnfConvertedExpected, nonConvertedExpected,
      nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  d.values IN ['A', 'C'] AND  ( d.values "
            "IN ['C', 'B', 'C']  OR d.values IN ['A', 'B'] ) ") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check DNF conversion disabled (with root disjunction)  but IN nodes processed
// (sorted and deduplicated)!
TEST_P(QueryOptimization, test_180) {
  auto dnfConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    {
      {
        auto& andFilter = root.add<irs::And>();
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& andFilter = root.add<irs::And>();
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& andFilter = root.add<irs::And>();
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    {
      auto& sub = root.add<irs::And>().add<irs::Or>();
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = sub.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    {
      auto& orFilter = root.add<irs::And>().add<irs::Or>();
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    {
      auto& orFilter = root.add<irs::And>().add<irs::Or>();
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      dnfConvertedExpected, dnfConvertedExpected, nonConvertedExpected,
      nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  d.values IN ['A', 'C'] OR  ( d.values "
            "IN ['C', 'B', 'C']  OR d.values IN ['A', 'B'] ) ") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check DNF conversion disabled (with root disjunction and conjunction inside)
// but IN nodes processed (sorted and deduplicated)!
TEST_P(QueryOptimization, test_181) {
  auto dnfConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    {
      auto& orFilter = root.add<irs::And>().add<irs::Or>();
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    {
      auto& andFilter = root.add<irs::And>();
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& part = andFilter.add<irs::Or>();
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = part.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    {
      auto& orFilter = expected.add<irs::And>().add<irs::Or>();
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = orFilter.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }
    }
    {
      auto& andFilter = expected.add<irs::And>();
      {
        auto& orFilter = andFilter.add<irs::Or>();
        {
          auto& filter = orFilter.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
        {
          auto& filter = orFilter.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("C"));
        }
      }
      {
        auto& orFilter = andFilter.add<irs::Or>();
        {
          auto& filter = orFilter.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("A"));
        }
        {
          auto& filter = orFilter.add<irs::by_term>();
          *filter.mutable_field() = mangleStringIdentity("values");
          filter.mutable_options()->term =
              irs::ViewCast<irs::byte_type>(std::string_view("B"));
        }
      }
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      dnfConvertedExpected, dnfConvertedExpected, nonConvertedExpected,
      nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  d.values IN ['A', 'C'] OR  ( d.values "
            "IN ['C', 'B', 'C']  AND d.values IN ['A', 'B'] ) ") +
        o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check Negation conversion disabled
TEST_P(QueryOptimization, test_182) {
  auto negationConvertedExpected = [](irs::Or& expected) {
    auto& root = expected;
    {
      auto& notFilter = root.add<irs::And>().add<irs::Not>();
      {
        auto& filter = notFilter.filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
    }
    {
      auto& notFilter = root.add<irs::And>().add<irs::Not>();
      {
        auto& filter = notFilter.filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    auto& root = expected.add<irs::And>()
                     .add<irs::Not>()
                     .filter<irs::And>()
                     .add<irs::And>();
    {
      auto& filter = root.add<irs::by_term>();
      *filter.mutable_field() = mangleStringIdentity("values");
      filter.mutable_options()->term =
          irs::ViewCast<irs::byte_type>(std::string_view("A"));
    }
    {
      auto& filter = root.add<irs::by_term>();
      *filter.mutable_field() = mangleStringIdentity("values");
      filter.mutable_options()->term =
          irs::ViewCast<irs::byte_type>(std::string_view("B"));
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      negationConvertedExpected, negationConvertedExpected,
      negationConvertedExpected, nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH  NOT (d.values == "
                                  "'A' AND  d.values == 'B') ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check Negation conversion disabled
TEST_P(QueryOptimization, test_183) {
  auto negationConvertedExpected = [](irs::Or& expected) {
    auto& root = expected.add<irs::And>();
    {
      auto& notFilter = root.add<irs::Not>();
      {
        auto& filter = notFilter.filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
    }
    {
      auto& notFilter = root.add<irs::Not>();
      {
        auto& filter = notFilter.filter<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
    }
  };

  auto nonConvertedExpected = [](irs::Or& expected) {
    auto& root = expected.add<irs::And>()
                     .add<irs::Not>()
                     .filter<irs::And>()
                     .add<irs::Or>();
    {
      auto& filter = root.add<irs::by_term>();
      *filter.mutable_field() = mangleStringIdentity("values");
      filter.mutable_options()->term =
          irs::ViewCast<irs::byte_type>(std::string_view("A"));
    }
    {
      auto& filter = root.add<irs::by_term>();
      *filter.mutable_field() = mangleStringIdentity("values");
      filter.mutable_options()->term =
          irs::ViewCast<irs::byte_type>(std::string_view("B"));
    }
  };

  std::vector<std::function<void(irs::Or&)>> structureChecks = {
      negationConvertedExpected, negationConvertedExpected,
      negationConvertedExpected, nonConvertedExpected, nonConvertedExpected};
  ASSERT_EQ(structureChecks.size(), optimizerOptionsAvailable.size());

  size_t structCheckIdx = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query = std::string(
                                  "FOR d IN testView SEARCH  NOT (d.values == "
                                  "'A' OR  d.values == 'B') ") +
                              o + "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure
    {
      irs::Or expected;
      structureChecks[structCheckIdx](expected);
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());

    structCheckIdx++;
  }
}

// check OR deduplication in sub-nodes
TEST_P(QueryOptimization, test_184) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  (d.values == 'A' OR d.values == 'B' OR "
            "d.values == 'A') AND  (d.values == 'A' OR d.values == 'C' OR "
            "d.values == 'C') ") +
        o + "RETURN d";
    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure only for non-optimized
    // Dnf-converter filter is out of scope, just run it and verify
    // returned documents are the same
    if (optimizeType >= disabledDnfOptimizationStart) {
      irs::Or expected;
      auto& andFilter = expected.add<irs::And>();
      auto& left = andFilter.add<irs::Or>();
      {
        auto& filter = left.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = left.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      auto& right = andFilter.add<irs::Or>();
      {
        auto& filter = right.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = right.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

// check IN deduplication in sub-nodes
TEST_P(QueryOptimization, test_185) {
  size_t optimizeType = 0;
  for (auto& o : optimizerOptionsAvailable) {
    SCOPED_TRACE(o);
    std::string const query =
        std::string(
            "FOR d IN testView SEARCH  (d.values IN ['A', 'B', 'A']) AND  "
            "(d.values == 'A' OR d.values == 'C' OR d.values == 'C') ") +
        o + "RETURN d";
    EXPECT_TRUE(arangodb::tests::assertRules(
        vocbase(), query,
        {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_FALSE(findEmptyNodes(vocbase(), query));
    // check structure only for non-optimized
    // Dnf-converter filter is out of scope, just run it and verify
    // returned documents are the same
    if (optimizeType >= disabledDnfOptimizationStart) {
      irs::Or expected;
      auto& andFilter = expected.add<irs::And>();
      auto& left = andFilter.add<irs::Or>();
      {
        auto& filter = left.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = left.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("B"));
      }
      auto& right = andFilter.add<irs::Or>();
      {
        auto& filter = right.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("A"));
      }
      {
        auto& filter = right.add<irs::by_term>();
        *filter.mutable_field() = mangleStringIdentity("values");
        filter.mutable_options()->term =
            irs::ViewCast<irs::byte_type>(std::string_view("C"));
      }

      assertFilterOptimized(vocbase(), query, expected);
    }
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0]->data()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_EQ(expectedDocs.size(), resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((
          0 == arangodb::basics::VelocyPackHelper::compare(
                   arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_EQ(expectedDoc, expectedDocs.end());
    ++optimizeType;
  }
}

TEST_P(QueryOptimization, mergeLevenshteinStartsWith) {
  // empty prefix case wrapped
  {
    irs::Or expected;
    auto& filter =
        expected.add<irs::And>().add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "test_analyzer");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH ANALYZER(LEVENSHTEIN_MATCH(d.name, 'foobar', "
        "2, false, 63) "
        "AND STARTS_WITH(d.name, 'foo'), 'test_analyzer') "
        "RETURN d",
        expected);
  }
  // empty prefix case unwrapped
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'foobar', 2, false, 63) "
                          "AND STARTS_WITH(d.name, 'foo') "
                          "RETURN d",
                          expected);
  }
  // full prefix match
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foo') "
                          "RETURN d",
                          expected);
  }
  // full prefix match + explicit allow
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foo') "
                          "OPTIONS {\"filterOptimization\": -1 } "
                          "RETURN d",
                          expected);
  }
  // substring prefix match
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'fo') "
                          "RETURN d",
                          expected);
  }
  // prefix enlargement case
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'obar', 2, false, 63, 'fo') "
                          "AND STARTS_WITH(d.name, 'foo') "
                          "RETURN d",
                          expected);
  }
  // prefix enlargement to the whole target
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foobar"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view(""));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'obar', 2, false, 63, 'fo') "
                          "AND STARTS_WITH(d.name, 'foobar') "
                          "RETURN d",
                          expected);
  }
  // empty prefix enlargement to the whole target
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foobar"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view(""));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'foobar', 2, false, 63) "
                          "AND STARTS_WITH(d.name, 'foobar') "
                          "RETURN d",
                          expected);
  }
  // make it empty with prefix
  {
    irs::Or expected;
    expected.add<irs::And>().add<irs::Empty>();
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH STARTS_WITH(d.name, 'foobar12345')"
        "AND LEVENSHTEIN_MATCH(d.name, 'bar', 2, false, 63, 'foo')"
        "RETURN d",
        expected);
  }
  // make it empty
  {
    irs::Or expected;
    expected.add<irs::And>().add<irs::Empty>();
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH STARTS_WITH(d.name, 'foobar12345')"
        "AND LEVENSHTEIN_MATCH(d.name, 'foobar', 2, false, 63)"
        "RETURN d",
        expected);
  }
  // empty prefix case - not match
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view(""));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("foobar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("boo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'foobar', 2, false, 63) "
                          "AND STARTS_WITH(d.name, 'boo') "
                          "RETURN d",
                          expected);
  }
  // prefix not match
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("boo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'boo') "
                          "RETURN d",
                          expected);
  }
  // prefix too long
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foobard"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foobard') "
                          "RETURN d",
                          expected);
  }
  // scorers block optimization
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foo') SORT BM25(d) "
                          "RETURN d",
                          expected);
  }
  // scorers block optimization + allow
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foo') OPTIONS "
                          "{\"filterOptimization\": -1} SORT BM25(d) "
                          "RETURN d",
                          expected);
  }
  // merging forbidden
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, 'foo') "
                          "OPTIONS {\"filterOptimization\": 0 } "
                          "RETURN d",
                          expected);
  }
  // multiprefixes is not merged
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& orFilter = andFilter.add<irs::Or>();
      orFilter.min_match_count(2);
      {
        auto& starts = orFilter.add<irs::by_prefix>();
        *starts.mutable_field() = mangleString("name", "identity");
        auto* opt = starts.mutable_options();
        opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foo"));
        opt->scored_terms_limit =
            arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
      }
      {
        auto& starts = orFilter.add<irs::by_prefix>();
        *starts.mutable_field() = mangleString("name", "identity");
        auto* opt = starts.mutable_options();
        opt->term = irs::ViewCast<irs::byte_type>(std::string_view("boo"));
        opt->scored_terms_limit =
            arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
      }
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'foo') "
                          "AND STARTS_WITH(d.name, ['foo', 'boo'], 2) "
                          "OPTIONS {\"filterOptimization\": 0 } "
                          "RETURN d",
                          expected);
  }
  // name not match
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("boo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("bar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name2", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("boo"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'bar', 2, false, 63, 'boo') "
                          "AND STARTS_WITH(d.name2, 'boo') "
                          "RETURN d",
                          expected);
  }
  // prefix could not be enlarged
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("fo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("obar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("foa"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'obar', 2, false, 63, 'fo') "
                          "AND STARTS_WITH(d.name, 'foa') "
                          "RETURN d",
                          expected);
  }
  // prefix could not be enlarged (prefix does not match)
  {
    irs::Or expected;
    auto& andFilter = expected.add<irs::And>();
    auto& filter = andFilter.add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    {
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("fo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("obar"));
      opts->with_transpositions = false;
    }
    {
      auto& starts = andFilter.add<irs::by_prefix>();
      *starts.mutable_field() = mangleString("name", "identity");
      auto* opt = starts.mutable_options();
      opt->term = irs::ViewCast<irs::byte_type>(std::string_view("fao"));
      opt->scored_terms_limit =
          arangodb::iresearch::FilterConstants::DefaultScoringTermsLimit;
    }
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'obar', 2, false, 63, 'fo') "
                          "AND STARTS_WITH(d.name, 'fao') "
                          "RETURN d",
                          expected);
  }
  // merge multiple
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foooab"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
    opts->with_transpositions = false;
    assertFilterOptimized(vocbase(),
                          "FOR d IN testView SEARCH LEVENSHTEIN_MATCH(d.name, "
                          "'r', 2, false, 63, 'foooab') "
                          " AND STARTS_WITH(d.name, 'f') "
                          " AND STARTS_WITH(d.name, 'foo')"
                          " AND STARTS_WITH(d.name, 'fo') "
                          " AND STARTS_WITH(d.name, 'foooab') "
                          " OPTIONS {\"conditionOptimization\":\"none\"} "
                          " RETURN d",
                          expected);
  }
  // merge multiple resort
  {
    irs::Or expected;
    auto& filter = expected.add<irs::And>().add<irs::by_edit_distance>();
    *filter.mutable_field() = mangleString("name", "identity");
    auto* opts = filter.mutable_options();
    opts->max_distance = 2;
    opts->max_terms = 63;
    opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foooab"));
    opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
    opts->with_transpositions = false;
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH "
        " STARTS_WITH(d.name, 'f') "
        " AND STARTS_WITH(d.name, 'foo')"
        " AND STARTS_WITH(d.name, 'fo') "
        " AND STARTS_WITH(d.name, 'foooab') "
        " AND LEVENSHTEIN_MATCH(d.name, 'r', 2, false, 63, 'foooab') "
        " OPTIONS {\"conditionOptimization\":\"none\"} "
        " RETURN d",
        expected);
  }
  // merge multiple resort 2 levs
  {
    irs::Or expected;
    auto& andF = expected.add<irs::And>();
    {
      auto& filter = andF.add<irs::by_edit_distance>();
      *filter.mutable_field() = mangleString("name", "identity");
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foooab"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
      opts->with_transpositions = false;
    }
    {
      auto& filter = andF.add<irs::by_edit_distance>();
      *filter.mutable_field() = mangleString("name", "identity");
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("poo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
      opts->with_transpositions = false;
    }
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH "
        " STARTS_WITH(d.name, 'f') "
        " AND STARTS_WITH(d.name, 'poo')"
        " AND LEVENSHTEIN_MATCH(d.name, 'poor', 2, false, 63) "
        " AND STARTS_WITH(d.name, 'fo') "
        " AND STARTS_WITH(d.name, 'foooab') "
        " AND LEVENSHTEIN_MATCH(d.name, 'r', 2, false, 63, 'foooab') "
        " OPTIONS {\"conditionOptimization\":\"none\"} "
        " RETURN d",
        expected);
  }
  // merge multiple resort 2 levs moar sorting
  {
    irs::Or expected;
    auto& andF = expected.add<irs::And>();
    {
      auto& filter = andF.add<irs::by_edit_distance>();
      *filter.mutable_field() = mangleString("name", "identity");
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("foooab"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
      opts->with_transpositions = false;
    }
    {
      auto& filter = andF.add<irs::by_edit_distance>();
      *filter.mutable_field() = mangleString("name", "identity");
      auto* opts = filter.mutable_options();
      opts->max_distance = 2;
      opts->max_terms = 63;
      opts->prefix = irs::ViewCast<irs::byte_type>(std::string_view("poo"));
      opts->term = irs::ViewCast<irs::byte_type>(std::string_view("r"));
      opts->with_transpositions = false;
    }
    assertFilterOptimized(
        vocbase(),
        "FOR d IN testView SEARCH "
        " STARTS_WITH(d.name, 'f') "
        " AND STARTS_WITH(d.name, 'poo')"
        " AND STARTS_WITH(d.name, 'fo') "
        " AND STARTS_WITH(d.name, 'foooab') "
        " AND LEVENSHTEIN_MATCH(d.name, 'poor', 2, false, 63) "
        " AND LEVENSHTEIN_MATCH(d.name, 'r', 2, false, 63, 'foooab') "
        " OPTIONS {\"conditionOptimization\":\"none\"} "
        " RETURN d",
        expected);
  }
}

INSTANTIATE_TEST_CASE_P(
    IResearch, QueryOptimization,
    testing::Values(std::tuple{ViewType::kArangoSearch,
                               arangodb::iresearch::LinkVersion::MIN},
                    std::tuple{ViewType::kArangoSearch,
                               arangodb::iresearch::LinkVersion::MAX},
                    std::tuple{ViewType::kSearchAlias,
                               arangodb::iresearch::LinkVersion::MAX}));

}  // namespace
}  // namespace arangodb::tests
