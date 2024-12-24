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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#include "Agency/AgencyComm.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/IResearchViewNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExpressionContext.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Async/async.h"
#include "Basics/FileUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/files.h"
#include "Basics/system-functions.h"
#include "ClusterEngine/ClusterEngine.h"
#include "ExpressionContextMock.h"
#include "IResearch/AqlHelper.h"
#include "IResearch/ExpressionFilter.h"
#include "IResearch/IResearchFilterContext.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchKludge.h"
#include "IResearch/VelocyPackHelper.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/KeyGenerator.h"
#include "common.h"
#include "gtest/gtest.h"

#include "index/index_reader.hpp"
#include "search/all_filter.hpp"
#include "search/boolean_filter.hpp"
#include "search/column_existence_filter.hpp"
#include "search/range_filter.hpp"
#include "search/terms_filter.hpp"
#include "search/nested_filter.hpp"
#include "search/scorers.hpp"
#include "search/term_filter.hpp"
#include "search/ngram_similarity_filter.hpp"
#include "search/levenshtein_filter.hpp"
#include "search/prefix_filter.hpp"
#include "search/granular_range_filter.hpp"
#include "utils/string.hpp"
#include <filesystem>

#include "../3rdParty/iresearch/tests/tests_config.hpp"

#ifdef USE_V8
#include <libplatform/libplatform.h>
#include <v8.h>
#endif

#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>

#include <absl/strings/ascii.h>

#include <unordered_set>

extern const char* ARGV0;  // defined in main.cpp

// GTEST printers for IResearch filters
namespace irs {
std::ostream& operator<<(std::ostream& os, filter const& filter);

std::string ToString(bytes_view term) {
  std::string s;
  for (auto c : term) {
    if (absl::ascii_isprint(c)) {
      s += c;
    } else {
      s += " ";
      s.append(absl::AlphaNum{int{c}}.Piece());
      s += " ";
    }
  }
  return s;
}

std::string ToString(std::vector<bstring> const& terms) {
  std::string s = "( ";
  for (auto& term : terms) {
    s += ToString(term) + " ";
  }
  s += ")";
  return s;
}

template<typename T>
std::ostream& operator<<(std::ostream& os, search_range<T> const& range) {
  if (!range.min.empty()) {
    os << " " << (range.min_type == irs::BoundType::INCLUSIVE ? ">=" : ">")
       << ToString(range.min);
  }
  if (!range.max.empty()) {
    if (!range.min.empty()) {
      os << ", ";
    } else {
      os << " ";
    }
    os << (range.min_type == irs::BoundType::INCLUSIVE ? "<=" : "<")
       << ToString(range.max);
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, by_range const& range) {
  return os << "Range(" << range.field() << range.options().range << ")";
}

std::ostream& operator<<(std::ostream& os, by_granular_range const& range) {
  return os << "GranularRange(" << range.field() << range.options().range
            << ")";
}

std::ostream& operator<<(std::ostream& os, by_term const& term) {
  std::string termValue(ViewCast<char>(irs::bytes_view{term.options().term}));
  return os << "Term(" << term.field() << "=" << termValue << ")";
}

std::ostream& operator<<(std::ostream& os, irs::ByNestedFilter const& filter) {
  auto& [parent, child, match, _] = filter.options();
  os << "NESTED[MATCH[";
  if (auto* range = std::get_if<irs::Match>(&match); range) {
    os << range->Min << ", " << range->Max;
  } else if (nullptr != std::get_if<irs::DocIteratorProvider>(&match)) {
    os << "<Predicate>";
  }
  os << "], CHILD[" << *child << "]]";
  return os;
}

std::ostream& operator<<(std::ostream& os, And const& filter) {
  os << "AND[";
  for (auto it = filter.begin(); it != filter.end(); ++it) {
    if (it != filter.begin()) {
      os << " && ";
    }
    os << **it;
  }
  os << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, Or const& filter) {
  os << "OR";
  if (filter.min_match_count() != 1) {
    os << "(" << filter.min_match_count() << ")";
  }
  os << "[";
  for (auto it = filter.begin(); it != filter.end(); ++it) {
    if (it != filter.begin()) {
      os << " || ";
    }
    os << **it;
  }
  os << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, Not const& filter) {
  os << "NOT[" << *filter.filter() << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, by_ngram_similarity const& filter) {
  os << "NGRAM_SIMILARITY[";
  os << filter.field() << ", ";
  for (auto const& ngram : filter.options().ngrams) {
    os << ngram.c_str();
  }
  os << "," << filter.options().threshold << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, Empty const&) {
  os << "EMPTY[]";
  return os;
}

std::ostream& operator<<(std::ostream& os,
                         irs::by_column_existence const& filter) {
  os << "EXISTS[" << filter.field() << ", " << size_t(filter.options().acceptor)
     << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, by_edit_distance const& lev) {
  os << "LEVENSHTEIN_MATCH[";
  os << lev.field() << ", '";
  std::string termValue(ViewCast<char>(irs::bytes_view{lev.options().term}));
  std::string prefixValue(
      ViewCast<char>(irs::bytes_view{lev.options().prefix}));
  os << termValue << "', " << static_cast<int>(lev.options().max_distance)
     << ", " << lev.options().with_transpositions << ", "
     << lev.options().max_terms << ", '" << prefixValue << "']";
  return os;
}

std::ostream& operator<<(std::ostream& os, by_prefix const& filter) {
  os << "STARTS_WITH[";
  os << filter.field() << ", '";
  std::string termValue(ViewCast<char>(irs::bytes_view{filter.options().term}));
  os << termValue << "', " << filter.options().scored_terms_limit << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, by_terms const& filter) {
  os << "TERMS[";
  os << filter.field() << ", {";
  for (auto& [term, boost] : filter.options().terms) {
    os << "['" << ViewCast<char>(irs::bytes_view{term}) << "', " << boost
       << "],";
  }
  os << "}, " << filter.options().min_match << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, all const& filter) {
  os << "ALL[";
  os << filter.boost();
  os << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, filter const& filter) {
  const auto& type = filter.type();
  if (type == irs::type<all>::id()) {
    return os << static_cast<all const&>(filter);
  } else if (type == irs::type<And>::id()) {
    return os << static_cast<And const&>(filter);
  } else if (type == irs::type<Or>::id()) {
    return os << static_cast<Or const&>(filter);
  } else if (type == irs::type<Not>::id()) {
    return os << static_cast<Not const&>(filter);
  } else if (type == irs::type<by_term>::id()) {
    return os << static_cast<by_term const&>(filter);
  } else if (type == irs::type<by_terms>::id()) {
    return os << static_cast<by_terms const&>(filter);
  } else if (type == irs::type<by_range>::id()) {
    return os << static_cast<by_range const&>(filter);
  } else if (type == irs::type<by_granular_range>::id()) {
    return os << static_cast<by_granular_range const&>(filter);
  } else if (type == irs::type<by_ngram_similarity>::id()) {
    return os << static_cast<by_ngram_similarity const&>(filter);
  } else if (type == irs::type<by_edit_distance>::id()) {
    return os << static_cast<by_edit_distance const&>(filter);
  } else if (type == irs::type<by_prefix>::id()) {
    return os << static_cast<by_prefix const&>(filter);
  } else if (type == irs::type<ByNestedFilter>::id()) {
    return os << static_cast<ByNestedFilter const&>(filter);
  } else if (type == irs::type<by_column_existence>::id()) {
    return os << static_cast<by_column_existence const&>(filter);
  } else if (type == irs::type<Empty>::id()) {
    return os << static_cast<Empty const&>(filter);
  } else if (type == irs::type<arangodb::iresearch::ByExpression>::id()) {
    return os << "ByExpression";
  } else {
    return os << "[Unknown filter " << type().name() << " ]";
  }
}

std::string to_string(irs::filter const& f) {
  std::stringstream ss;
  ss << f;
  return ss.str();
}
}  // namespace irs

namespace {

struct BoostScorer final : public irs::ScorerBase<void> {
  static constexpr std::string_view type_name() noexcept {
    return "boostscorer";
  }

  void collect(irs::byte_type* stats, irs::FieldCollector const*,
               irs::TermCollector const*) const final {
    // NOOP
  }

  irs::IndexFeatures index_features() const final {
    return irs::IndexFeatures::NONE;
  }

  irs::FieldCollector::ptr prepare_field_collector() const final {
    return nullptr;
  }

  irs::TermCollector::ptr prepare_term_collector() const final {
    return nullptr;
  }

  irs::ScoreFunction prepare_scorer(
      irs::ColumnProvider const&,
      std::map<irs::type_info::type_id, irs::field_id> const&,
      irs::byte_type const* stats, irs::attribute_provider const&,
      irs::score_t boost) const final {
    struct ScoreCtx final : public irs::score_ctx {
      explicit ScoreCtx(irs::score_t b) noexcept : boost{b} {}

      irs::score_t boost;
    };

    return irs::ScoreFunction::Make<ScoreCtx>(
        [](irs::score_ctx* ctx, irs::score_t* res) noexcept {
          *res = static_cast<ScoreCtx*>(ctx)->boost;
        },
        irs::ScoreFunction::DefaultMin, boost);
  }

  static irs::Scorer::ptr make(std::string_view) {
    return std::make_unique<BoostScorer>();
  }

  BoostScorer() = default;
};

REGISTER_SCORER_JSON(BoostScorer, BoostScorer::make);

struct CustomScorer final : public irs::ScorerBase<void> {
  static constexpr std::string_view type_name() noexcept {
    return "customscorer";
  }

  void collect(irs::byte_type*, irs::FieldCollector const*,
               irs::TermCollector const*) const final {
    // NOOP
  }

  irs::IndexFeatures index_features() const final {
    return irs::IndexFeatures::NONE;
  }

  irs::FieldCollector::ptr prepare_field_collector() const final {
    return nullptr;
  }

  irs::TermCollector::ptr prepare_term_collector() const final {
    return nullptr;
  }

  irs::ScoreFunction prepare_scorer(
      irs::ColumnProvider const&,
      std::map<irs::type_info::type_id, irs::field_id> const&,
      irs::byte_type const* stats, irs::attribute_provider const&,
      irs::score_t) const final {
    struct ScoreCtx : public irs::score_ctx {
      ScoreCtx(float v) noexcept : scoreValue{v} {}

      float scoreValue;
    };

    return irs::ScoreFunction::Make<ScoreCtx>(
        [](irs::score_ctx* ctx, irs::score_t* res) noexcept {
          *res = static_cast<ScoreCtx const*>(ctx)->scoreValue;
        },
        irs::ScoreFunction::DefaultMin, this->i);
  }

  bool equals(irs::Scorer const& other) const noexcept final {
    if (!Scorer::equals(other)) {
      return false;
    }
    auto const& p = DownCast<CustomScorer>(other);
    return p.i == i;
  }

  static irs::Scorer::ptr make(std::string_view args) {
    if (irs::IsNull(args)) {
      return std::make_unique<CustomScorer>(0u);
    }

    // velocypack::Parser::fromJson(...) will throw exception on parse error
    auto json =
        arangodb::velocypack::Parser::fromJson(args.data(), args.size());
    auto slice = json ? json->slice() : arangodb::velocypack::Slice();

    if (!slice.isArray()) {
      return nullptr;  // incorrect argument format
    }

    arangodb::velocypack::ArrayIterator itr{slice};

    if (!itr.valid()) {
      return nullptr;
    }

    auto const value = itr.value();

    if (!value.isNumber<size_t>()) {
      return nullptr;
    }
    return std::make_unique<CustomScorer>(value.getNumber<size_t>());
  }

  CustomScorer(size_t i) noexcept : i{static_cast<float>(i)} {}

  float i;
};

REGISTER_SCORER_JSON(CustomScorer, CustomScorer::make);

static const VPackBuilder testDatabaseBuilder = dbArgsBuilder("testVocbase");
static const VPackSlice testDatabaseArgs = testDatabaseBuilder.slice();
}  // namespace

namespace arangodb {

namespace tests {

void waitForAsync(async<void>&& as) {
  [&]() -> futures::Future<futures::Unit> {
    co_return co_await std::move(as);
  }()
               .waitAndGet();
}

void checkQuery(TRI_vocbase_t& vocbase,
                std::span<const velocypack::Slice> expected,
                std::string const& query) {
  auto result = tests::executeQuery(vocbase, query);
  SCOPED_TRACE(testing::Message("Error: ") << result.errorMessage());
  ASSERT_TRUE(result.result.ok());
  auto slice = result.data->slice();
  EXPECT_TRUE(slice.isArray());
  size_t i = 0;

  for (velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
    auto const resolved = itr.value().resolveExternals();
    EXPECT_TRUE(i < expected.size());
    EXPECT_EQUAL_SLICES(expected[i++], resolved);
  }

  EXPECT_EQ(i, expected.size());
}

std::string const AnalyzerCollectionName("_analyzers");

std::string testResourceDir;

static void findIResearchTestResources() {
  std::string toBeFound = basics::FileUtils::buildFilename(
      "3rdParty", "iresearch", "tests", "resources");

  // peek into environment variable first
  char const* dir = getenv("IRESEARCH_TEST_RESOURCE_DIR");
  if (dir != nullptr) {
    // environment variable set, so use it
    testResourceDir = std::string(dir);
  } else {
    // environment variable not set, so try to auto-detect the location
    testResourceDir = ".";
    do {
      if (basics::FileUtils::isDirectory(
              basics::FileUtils::buildFilename(testResourceDir, toBeFound))) {
        testResourceDir =
            basics::FileUtils::buildFilename(testResourceDir, toBeFound);
        return;
      }
      testResourceDir = basics::FileUtils::buildFilename(testResourceDir, "..");
      if (!basics::FileUtils::isDirectory(testResourceDir)) {
        testResourceDir = IResearch_test_resource_dir;
        break;
      }
    } while (true);
  }

  if (!basics::FileUtils::isDirectory(testResourceDir)) {
    LOG_TOPIC("45f9d", ERR, Logger::FIXME)
        << "unable to find directory for IResearch test resources. use "
           "environment variable IRESEARCH_TEST_RESOURCE_DIR to set it";
  }
}

void init(bool withICU /*= false*/) {
  arangodb::transaction::Methods::clearDataSourceRegistrationCallbacks();
  ClusterEngine::Mocking = true;
  arangodb::RandomGenerator::initialize(
      arangodb::RandomGenerator::RandomType::MERSENNE);

  // try to locate directory for iresearch test resource files
  if (testResourceDir.empty()) {
    findIResearchTestResources();
  }
}

#ifdef USE_V8
/// @note once V8 is initialized all 'CATCH' errors will result in SIGILL
void v8Init() {
  class V8Init {
   public:
    V8Init() {
      _platform = v8::platform::NewDefaultPlatform();
      // avoid SIGSEGV during v8::Isolate::New(...)
      v8::V8::InitializePlatform(_platform.get());
      // avoid error: "Check failed: thread_data_table_"
      v8::V8::Initialize();
    }
    ~V8Init() {
      v8::V8::Dispose();
      v8::V8::DisposePlatform();
    }

   private:
    std::unique_ptr<v8::Platform> _platform;
  };
  [[maybe_unused]] static const V8Init init;
}
#endif

bool assertRules(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::vector<int> const& expectedRulesIds,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /* = nullptr */,
    std::string const& optionsString /*= "{}"*/
) {
  std::unordered_set<std::string> expectedRules;
  for (auto ruleId : expectedRulesIds) {
    std::string_view translated =
        arangodb::aql::OptimizerRulesFeature::translateRule(ruleId);
    expectedRules.emplace(std::string(translated));
  }
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      ctx, arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(
          arangodb::velocypack::Parser::fromJson(optionsString)->slice()));

  auto const res = query->explain();

  if (res.data) {
    auto const explanation = res.data->slice();

    arangodb::velocypack::ArrayIterator rules(explanation.get("rules"));

    for (auto const& rule : rules) {
      expectedRules.erase(rule.copyString());
    }
  }

  // note: expectedRules may also not be empty because the query failed.
  // assertRules does not report failed queries so far.
  return expectedRules.empty();
}

arangodb::aql::QueryResult explainQuery(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& optionsString /*= "{}"*/) {
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(
          arangodb::velocypack::Parser::fromJson(optionsString)->slice()));

  return query->explain();
}

arangodb::aql::QueryResult executeQuery(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& optionsString /*= "{}"*/
) {
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars,
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

std::unique_ptr<arangodb::aql::ExecutionPlan> planFromQuery(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /* = nullptr */,
    std::string const& optionsString /*= "{}"*/) {
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(
          arangodb::velocypack::Parser::fromJson(optionsString)->slice()));
  query->initTrxForTests();

  auto result = query->parse();

  if (result.result.fail() || !query->ast()) {
    return nullptr;
  }

  return arangodb::aql::ExecutionPlan::instantiateFromAst(query->ast(), false);
}

std::shared_ptr<arangodb::aql::Query> prepareQuery(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /* = nullptr */,
    std::string const& optionsString /*= "{}"*/) {
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(
          arangodb::velocypack::Parser::fromJson(optionsString)->slice()));

  waitForAsync(query->prepareQuery());
  return query;
}

uint64_t getCurrentPlanVersion(arangodb::ArangodServer& server) {
  auto const result = arangodb::AgencyComm(server).getValues("Plan");
  auto const planVersionSlice = result.slice()[0].get<std::string>(
      {arangodb::AgencyCommHelper::path(), "Plan", "Version"});
  return planVersionSlice.getNumber<uint64_t>();
}

void setDatabasePath(arangodb::DatabasePathFeature& feature) {
  std::filesystem::path path;

  path /= TRI_GetTempPath();
  path /= std::string("arangodb_tests.") + std::to_string(TRI_microtime());
  feature.setDirectory(path.string());
}

void expectEqualSlices_(const VPackSlice& lhs, const VPackSlice& rhs,
                        const char* where) {
  SCOPED_TRACE(rhs.toString());
  SCOPED_TRACE(rhs.toHex());
  SCOPED_TRACE("----ACTUAL----");
  SCOPED_TRACE(lhs.toString());
  SCOPED_TRACE(lhs.toHex());
  SCOPED_TRACE("---EXPECTED---");
  SCOPED_TRACE(where);
  EXPECT_EQ(0, arangodb::basics::VelocyPackHelper::compare(lhs, rhs, true));
}

}  // namespace tests
}  // namespace arangodb

using namespace arangodb;

std::string mangleType(std::string name) {
  arangodb::iresearch::kludge::mangleType(name);
  return name;
}

std::string mangleAnalyzer(std::string name) {
  arangodb::iresearch::kludge::mangleAnalyzer(name);
  return name;
}

std::string mangleBool(std::string name) {
  arangodb::iresearch::kludge::mangleBool(name);
  return name;
}

std::string mangleNull(std::string name) {
  arangodb::iresearch::kludge::mangleNull(name);
  return name;
}

std::string mangleNumeric(std::string name) {
  arangodb::iresearch::kludge::mangleNumeric(name);
  return name;
}

std::string mangleNested(std::string name) {
  arangodb::iresearch::kludge::mangleNested(name);
  return name;
}

std::string mangleString(std::string name, std::string_view suffix) {
  arangodb::iresearch::kludge::mangleAnalyzer(name);
  name += suffix;
  return name;
}

std::string mangleStringIdentity(std::string name) {
  arangodb::iresearch::kludge::mangleField(
      name, true,
      arangodb::iresearch::FieldMeta::Analyzer{
          arangodb::iresearch::IResearchAnalyzerFeature::identity()});

  return name;
}

std::string mangleInvertedIndexStringIdentity(std::string name) {
  arangodb::iresearch::kludge::mangleField(
      name, false,
      arangodb::iresearch::FieldMeta::Analyzer{
          arangodb::iresearch::IResearchAnalyzerFeature::identity()});

  return name;
}

void assertFilterOptimized(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    irs::filter const& expectedFilter,
    arangodb::aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /* = nullptr */) {
  auto options = arangodb::velocypack::Parser::fromJson(
      //    "{ \"tracing\" : 1 }"
      " { } ");

  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars,
      arangodb::aql::QueryOptions(options->slice()));

  tests::waitForAsync(query->prepareQuery());
  EXPECT_TRUE(query->plan());
  auto plan = const_cast<arangodb::aql::ExecutionPlan*>(query->plan());

  arangodb::containers::SmallVector<arangodb::aql::ExecutionNode*, 8> nodes;
  plan->findNodesOfType(
      nodes, arangodb::aql::ExecutionNode::ENUMERATE_IRESEARCH_VIEW, true);

  EXPECT_EQ(nodes.size(), 1U);

  auto* viewNode = arangodb::aql::ExecutionNode::castTo<
      arangodb::iresearch::IResearchViewNode*>(nodes.front());

  EXPECT_TRUE(viewNode);

  // execution time
  {
    arangodb::transaction ::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, arangodb::transaction::Options());

    auto* mockCtx = dynamic_cast<ExpressionContextMock*>(exprCtx);
    if (mockCtx) {  // simon: hack to make expression context work again
      mockCtx->setTrx(&trx);
    }

    irs::Or actualFilter;
    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx,
        .ast = plan->getAst(),
        .ctx = exprCtx,
        .index = &irs::SubReader::empty(),
        .ref = &viewNode->outVariable(),
        .filterOptimization = viewNode->filterOptimization(),
        .isSearchQuery = true};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    EXPECT_TRUE(arangodb::iresearch::FilterFactory::filter(
                    &actualFilter, filterCtx, viewNode->filterCondition())
                    .ok());
    EXPECT_FALSE(actualFilter.empty());
    EXPECT_EQ(expectedFilter, **actualFilter.begin());
  }
}

void assertExpressionFilter(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    irs::score_t boost /*= irs::kNoBoost*/,
    std::function<arangodb::aql::AstNode*(arangodb::aql::AstNode*)> const&
        expressionExtractor /*= &defaultExpressionExtractor*/,
    std::string const& refName /*= "d"*/) {
  SCOPED_TRACE(testing::Message("assertExpressionFilter failed for query: <")
               << queryString << ">");

  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), nullptr);

  auto const parseResult = query->parse();
  ASSERT_TRUE(parseResult.result.ok());

  auto* ast = query->ast();
  ASSERT_TRUE(ast);

  auto* root = ast->root();
  ASSERT_TRUE(root);

  // find first FILTER node
  arangodb::aql::AstNode* filterNode = nullptr;
  for (size_t i = 0; i < root->numMembers(); ++i) {
    auto* node = root->getMemberUnchecked(i);
    ASSERT_TRUE(node);

    if (arangodb::aql::NODE_TYPE_FILTER == node->type) {
      filterNode = node;
      break;
    }
  }
  ASSERT_TRUE(filterNode);

  // find referenced variable
  auto* allVars = ast->variables();
  ASSERT_TRUE(allVars);
  arangodb::aql::Variable* ref = nullptr;
  for (auto const& entry : allVars->variables(true)) {
    if (entry.second == refName) {
      ref = allVars->getVariable(entry.first);
      break;
    }
  }
  ASSERT_TRUE(ref);

  // supportsFilterCondition
  {
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, arangodb::transaction::Options());
    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx, .ref = ref, .isSearchQuery = true};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    EXPECT_TRUE((arangodb::iresearch::FilterFactory::filter(nullptr, filterCtx,
                                                            *filterNode)
                     .ok()));
  }

  // iteratorForCondition
  {
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, arangodb::transaction::Options());

    ExpressionContextMock exprCtx;
    exprCtx.setTrx(&trx);

    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx,
        .ast = ast,
        .ctx = &exprCtx,
        .index = &irs::SubReader::empty(),
        .ref = ref,
        .isSearchQuery = true};

    irs::Or expected;
    expected.add<arangodb::iresearch::ByExpression>().init(
        ctx, *expressionExtractor(filterNode));

    irs::Or actual;
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    EXPECT_TRUE(arangodb::iresearch::FilterFactory::filter(&actual, filterCtx,
                                                           *filterNode)
                    .ok());
    EXPECT_EQ(expected, actual) << to_string(expected) << "\n"
                                << to_string(actual);
    EXPECT_EQ(boost, (*actual.begin())->BoostImpl());
  }
}

static bool assertFilterBoostImpl(irs::filter const& expected,
                                  irs::filter const& actual) {
  if (expected.BoostImpl() != actual.BoostImpl()) {
    return false;
  }
  auto* expectedBooleanFilter =
      dynamic_cast<irs::boolean_filter const*>(&expected);
  if (expectedBooleanFilter) {
    auto* actualBooleanFilter =
        dynamic_cast<irs::boolean_filter const*>(&actual);
    if (actualBooleanFilter == nullptr ||
        expectedBooleanFilter->size() != actualBooleanFilter->size()) {
      return false;
    }

    auto expectedBegin = expectedBooleanFilter->begin();
    auto expectedEnd = expectedBooleanFilter->end();

    for (auto actualBegin = actualBooleanFilter->begin();
         expectedBegin != expectedEnd;) {
      if (!assertFilterBoostImpl(**expectedBegin, **actualBegin)) {
        return false;
      }
      ++expectedBegin;
      ++actualBegin;
    }

    return true;
  }
  auto* expectedNegationFilter = dynamic_cast<irs::Not const*>(&expected);
  if (expectedNegationFilter) {
    auto* actualNegationFilter = dynamic_cast<irs::Not const*>(&actual);
    return actualNegationFilter != nullptr &&
           assertFilterBoostImpl(*expectedNegationFilter->filter(),
                                 *actualNegationFilter->filter());
  }
  return true;
}
void assertFilterBoost(irs::filter const& expected, irs::filter const& actual) {
  EXPECT_TRUE(assertFilterBoostImpl(expected, actual));
}

void buildActualFilter(
    TRI_vocbase_t& vocbase, std::string const& queryString, irs::filter& actual,
    arangodb::aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& refName /*= "d"*/
) {
  auto ctx = std::make_shared<arangodb::transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = arangodb::aql::Query::create(
      std::move(ctx), arangodb::aql::QueryString(queryString), bindVars);

  auto const parseResult = query->parse();
  ASSERT_TRUE(parseResult.result.ok());

  auto* ast = query->ast();
  ASSERT_TRUE(ast);

  auto* root = ast->root();
  ASSERT_TRUE(root);

  // find first FILTER node
  arangodb::aql::AstNode* filterNode = nullptr;
  for (size_t i = 0; i < root->numMembers(); ++i) {
    auto* node = root->getMemberUnchecked(i);
    ASSERT_TRUE(node);

    if (arangodb::aql::NODE_TYPE_FILTER == node->type) {
      filterNode = node;
      break;
    }
  }
  ASSERT_TRUE(filterNode);

  // find referenced variable
  auto* allVars = ast->variables();
  ASSERT_TRUE(allVars);
  arangodb::aql::Variable* ref = nullptr;
  for (auto const& entry : allVars->variables(true)) {
    if (entry.second == refName) {
      ref = allVars->getVariable(entry.first);
      break;
    }
  }
  ASSERT_TRUE(ref);

  // optimization time
  {
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, arangodb::transaction::Options());

    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx, .ref = ref, .isSearchQuery = true};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    ASSERT_TRUE(arangodb::iresearch::FilterFactory::filter(nullptr, filterCtx,
                                                           *filterNode)
                    .ok());
  }

  // execution time
  {
    arangodb::transaction ::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, arangodb::transaction::Options());

    auto* mockCtx = dynamic_cast<ExpressionContextMock*>(exprCtx);
    if (mockCtx) {  // simon: hack to make expression context work again
      mockCtx->setTrx(&trx);
    }

    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx,
        .ast = ast,
        .ctx = exprCtx,
        .index = &irs::SubReader::empty(),
        .ref = ref,
        .isSearchQuery = true};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    ASSERT_TRUE(
        arangodb::iresearch::FilterFactory::filter(
            dynamic_cast<irs::boolean_filter*>(&actual), filterCtx, *filterNode)
            .ok());
  }
}

void assertFilter(
    TRI_vocbase_t& vocbase, bool parseOk, bool execOk,
    std::string const& queryString, irs::filter const& expected,
    aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<arangodb::velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& refName /*= "d"*/,
    arangodb::iresearch::FilterOptimization filterOptimization /*= NONE*/,
    bool searchQuery /*= true*/, bool oldMangling /*= true*/,
    bool hasNested /*= false*/) {
  SCOPED_TRACE(testing::Message("assertFilter failed for query:<")
               << queryString << "> parseOk:" << parseOk
               << " execOk:" << execOk);

  auto ctx = std::make_shared<transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = aql::Query::create(std::move(ctx), aql::QueryString(queryString),
                                  bindVars);
  auto const parseResult = query->parse();
  ASSERT_TRUE(parseResult.result.ok());

  auto* ast = query->ast();
  ASSERT_TRUE(ast);

  auto* root = ast->root();
  ASSERT_TRUE(root);

  // find first FILTER node
  aql::AstNode* filterNode = nullptr;
  for (size_t i = 0; i < root->numMembers(); ++i) {
    auto* node = root->getMemberUnchecked(i);
    ASSERT_TRUE(node);

    if (aql::NODE_TYPE_FILTER == node->type) {
      filterNode = node;
      break;
    }
  }
  ASSERT_TRUE(filterNode);

  // find referenced variable
  auto* allVars = ast->variables();
  ASSERT_TRUE(allVars);
  aql::Variable* ref = nullptr;
  for (auto const& entry : allVars->variables(true)) {
    if (entry.second == refName) {
      ref = allVars->getVariable(entry.first);
      break;
    }
  }
  ASSERT_TRUE(ref);

  // optimization time
  {
    transaction::Methods trx(
        transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, transaction::Options());

    auto* mockCtx = dynamic_cast<ExpressionContextMock*>(exprCtx);
    if (mockCtx) {  // simon: hack to make expression context work again
      mockCtx->setTrx(&trx);
    }

    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx,
        .ref = ref,
        .filterOptimization = filterOptimization,
        .namePrefix = arangodb::iresearch::nestedRoot(hasNested),
        .isSearchQuery = searchQuery,
        .isOldMangling = oldMangling};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    EXPECT_EQ(parseOk, arangodb::iresearch::FilterFactory::filter(
                           nullptr, filterCtx, *filterNode)
                           .ok());
  }

  // execution time
  {
    transaction::Methods trx(
        transaction::StandaloneContext::create(
            vocbase, transaction::OperationOriginTestCase{}),
        {}, {}, {}, transaction::Options());

    auto* mockCtx = dynamic_cast<ExpressionContextMock*>(exprCtx);
    if (mockCtx) {  // simon: hack to make expression context work again
      mockCtx->setTrx(&trx);
    }

    irs::Or actual;
    arangodb::iresearch::QueryContext const ctx{
        .trx = &trx,
        .ast = ast,
        .ctx = exprCtx,
        .index = &irs::SubReader::empty(),
        .ref = ref,
        .filterOptimization = filterOptimization,
        .namePrefix = arangodb::iresearch::nestedRoot(hasNested),
        .isSearchQuery = searchQuery,
        .isOldMangling = oldMangling};
    arangodb::iresearch::FieldMeta::Analyzer analyzer{
        arangodb::iresearch::IResearchAnalyzerFeature::identity()};
    arangodb::iresearch::FilterContext const filterCtx{
        .query = ctx, .contextAnalyzer = analyzer};
    auto r = arangodb::iresearch::FilterFactory::filter(&actual, filterCtx,
                                                        *filterNode);
    if (execOk) {
      EXPECT_TRUE(r.ok()) << r.errorMessage();
      if (r.ok()) {
        if (expected != actual) {
          std::cerr << expected << std::endl;
          std::cerr << actual << std::endl;
        }
        EXPECT_TRUE(assertFilterBoostImpl(expected, actual));
      }
    } else {
      EXPECT_FALSE(r.ok());
    }
  }
}

void assertFilterSuccess(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    irs::filter const& expected, aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& refName /*= "d"*/,
    arangodb::iresearch::FilterOptimization filterOptimization /*= NONE*/,
    bool searchQuery /*= true*/, bool oldMangling /*= true*/,
    bool hasNested /*= false*/) {
  return assertFilter(vocbase, true, true, queryString, expected, exprCtx,
                      bindVars, refName, filterOptimization, searchQuery,
                      oldMangling, hasNested);
}

void assertFilterFail(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& refName /*= "d"*/) {
  irs::Or expected;
  return assertFilter(vocbase, false, false, queryString, expected, exprCtx,
                      bindVars, refName);
}

void assertFilterExecutionFail(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    aql::ExpressionContext* exprCtx /*= nullptr*/,
    std::shared_ptr<velocypack::Builder> bindVars /*= nullptr*/,
    std::string const& refName /*= "d"*/) {
  irs::Or expected;
  return assertFilter(vocbase, true, false, queryString, expected, exprCtx,
                      bindVars, refName);
}

void assertFilterParseFail(
    TRI_vocbase_t& vocbase, std::string const& queryString,
    std::shared_ptr<velocypack::Builder> bindVars /*= nullptr*/) {
  SCOPED_TRACE(testing::Message("assertFilterParseFail failed for query:<")
               << queryString << ">");
  auto ctx = std::make_shared<transaction::StandaloneContext>(
      vocbase, transaction::OperationOriginTestCase{});
  auto query = aql::Query::create(std::move(ctx), aql::QueryString(queryString),
                                  bindVars);

  auto const parseResult = query->parse();
  ASSERT_TRUE(parseResult.result.fail());
}

VPackBuilder getInvertedIndexPropertiesSlice(
    IndexId iid, std::vector<std::string> const& fields,
    std::vector<std::vector<std::string>> const* storedFields,
    std::vector<std::pair<std::string, bool>> const* sortedFields,
    std::string_view name) {
  VPackBuilder vpack;
  {
    VPackObjectBuilder obj(&vpack);
    if (!name.empty()) {
      vpack.add(arangodb::StaticStrings::IndexName, VPackValue{name});
    }
    vpack.add(arangodb::StaticStrings::IndexId, VPackValue(iid.id()));
    vpack.add(arangodb::StaticStrings::IndexType, VPackValue("inverted"));

    // FIXME: maybe this should be set by index internally ?
    vpack.add(arangodb::StaticStrings::IndexUnique, VPackValue(false));
    vpack.add(arangodb::StaticStrings::IndexSparse, VPackValue(true));

    {
      VPackArrayBuilder arrayFields(&vpack,
                                    arangodb::StaticStrings::IndexFields);
      for (auto const& f : fields) {
        vpack.add(VPackValue(f));
      }
    }
    if (storedFields && !storedFields->empty()) {
      VPackArrayBuilder arrayFields(&vpack, "storedValues");
      for (auto const& f : *storedFields) {
        VPackArrayBuilder arrayFields(&vpack);
        for (auto const& s : f) {
          vpack.add(VPackValue(s));
        }
      }
    }

    if (sortedFields && !sortedFields->empty()) {
      VPackObjectBuilder arraySort(&vpack, "primarySort");
      VPackBuilder fields;
      {
        VPackArrayBuilder arr(&fields);
        for (auto const& f : *sortedFields) {
          VPackObjectBuilder field(&fields);
          fields.add("field", VPackValue(f.first));
          fields.add("direction", VPackValue(f.second ? "asc" : "desc"));
        }
      }
      vpack.add("fields", fields.slice());
    }
  }
  return vpack;
}

CreateDatabaseInfo createInfo(ArangodServer& server, std::string const& name,
                              uint64_t id) {
  CreateDatabaseInfo info(server, ExecContext::current());
  auto rv = info.load(name, id);
  if (rv.fail()) {
    throw std::runtime_error(rv.errorMessage().data());
  }
  return info;
}

CreateDatabaseInfo systemDBInfo(ArangodServer& server, std::string const& name,
                                uint64_t id) {
  return createInfo(server, name, id);
}

CreateDatabaseInfo testDBInfo(ArangodServer& server, std::string const& name,
                              uint64_t id) {
  return createInfo(server, name, id);
}

CreateDatabaseInfo unknownDBInfo(ArangodServer& server, std::string const& name,
                                 uint64_t id) {
  return createInfo(server, name, id);
}
