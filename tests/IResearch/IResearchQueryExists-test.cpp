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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "common.h"

#include "StorageEngineMock.h"

#if USE_ENTERPRISE
  #include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "V8/v8-globals.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"
#include "Transaction/UserTransaction.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/OptimizerRulesFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchView.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/SystemDatabaseFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogTopic.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "ApplicationFeatures/JemallocFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "Basics/VelocyPackHelper.h"
#include "Aql/Ast.h"
#include "Aql/Query.h"
#include "3rdParty/iresearch/tests/tests_config.hpp"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0; // defined in main.cpp

NS_LOCAL

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchQuerySetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;

  IResearchQuerySetup(): server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init(true);

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::WARN);

    // setup required application features
    features.emplace_back(new arangodb::ViewTypesFeature(&server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(&server), true);
    features.emplace_back(new arangodb::DatabasePathFeature(&server), false);
    features.emplace_back(new arangodb::JemallocFeature(&server), false); // required for DatabasePathFeature
    features.emplace_back(new arangodb::DatabaseFeature(&server), false);
    features.emplace_back(new arangodb::QueryRegistryFeature(&server), false); // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(features.back().first);
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(&server), false); // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(&server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(&server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(&server), true); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::SystemDatabaseFeature(&server, system.get()), false); // required for IResearchAnalyzerFeature

    #if USE_ENTERPRISE
      features.emplace_back(new arangodb::LdapFeature(&server), false); // required for AuthenticationFeature with USE_ENTERPRISE
    #endif

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    auto* analyzers = arangodb::iresearch::getFeature<arangodb::iresearch::IResearchAnalyzerFeature>();

    analyzers->emplace("test_analyzer", "TestAnalyzer", "abc"); // cache analyzer
    analyzers->emplace("test_csv_analyzer", "TestDelimAnalyzer", ","); // cache analyzer

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::ERR); // suppress WARNING DefaultCustomTypeHandler called
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);
  }

  ~IResearchQuerySetup() {
    system.reset(); // destroy before reseting the 'ENGINE'
    arangodb::AqlFeature(&server).stop(); // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::DEFAULT);
  }
}; // IResearchQuerySetup

NS_END

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchQueryTestExists", "[iresearch][iresearch-query]") {
  IResearchQuerySetup s;
  UNUSED(s);

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  std::vector<arangodb::velocypack::Builder> insertedDocs;
  arangodb::LogicalView* view;

  // create collection0
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection0\" }");
    auto* collection = vocbase.createCollection(createJson->slice());
    REQUIRE((nullptr != collection));

    std::vector<std::shared_ptr<arangodb::velocypack::Builder>> docs {
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -6, \"value\": null }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -5, \"value\": true }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -4, \"value\": \"abc\" }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -3, \"value\": 3.14 }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -2, \"value\": [ 1, \"abc\" ] }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -1, \"value\": { \"a\": 7, \"b\": \"c\" } }"),
    };

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      collection->cid(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));

    for (auto& entry: docs) {
      auto res = trx.insert(collection->name(), entry->slice(), options);
      CHECK((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    CHECK((trx.commit().ok()));
  }

  // create collection1
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection1\" }");
    auto* collection = vocbase.createCollection(createJson->slice());
    REQUIRE((nullptr != collection));

    irs::utf8_path resource;
    resource/=irs::string_ref(IResearch_test_resource_dir);
    resource/=irs::string_ref("simple_sequential.json");

    auto builder = arangodb::basics::VelocyPackHelper::velocyPackFromFile(resource.utf8());
    auto slice = builder.slice();
    REQUIRE(slice.isArray());

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      collection->cid(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto res = trx.insert(collection->name(), itr.value(), options);
      CHECK((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    CHECK((trx.commit().ok()));
  }

  // create view
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));

    view = logicalView.get();
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view->getImplementation());
    REQUIRE((false == !impl));

    auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
        "\"testCollection0\": { \"includeAllFields\": true, \"trackListPositions\": true },"
        "\"testCollection1\": { \"includeAllFields\": true }"
      "}}"
    );
    CHECK((impl->updateProperties(updateJson->slice(), true, false).ok()));
    std::set<TRI_voc_cid_t> cids;
    impl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
    CHECK((2 == cids.size()));
    impl->sync();
  }

  // test non-existent (any)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.missing) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (any) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['missing']) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (bool)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.name, 'type', 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (bool) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['name'], 'type', 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (boolean)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.name, 'type', 'boolean') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (boolean) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['name'], 'type', 'boolean') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (numeric)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.name, 'type', 'numeric') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (numeric) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['name'], 'type', 'numeric') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (null)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.name, 'type', 'null') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (null) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['name'], 'type', 'null') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (string)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.seq, 'type', 'string') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (string) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['seq'], 'type', 'string') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (analyzer)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.seq, 'analyzer', 'text_en') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (analyzer) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['seq'], 'analyzer', 'text_en') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (array)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value[2]) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (array) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'][2]) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (object)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value.d) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test non-existent (object) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value']['d']) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (any)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[0].slice(),
      insertedDocs[1].slice(),
      insertedDocs[2].slice(),
      insertedDocs[3].slice(),
      insertedDocs[4].slice(),
      insertedDocs[5].slice(),
      insertedDocs[6].slice(),
      insertedDocs[7].slice(),
      insertedDocs[8].slice(),
      insertedDocs[9].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[13].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[19].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[22].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (any) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[0].slice(),
      insertedDocs[1].slice(),
      insertedDocs[2].slice(),
      insertedDocs[3].slice(),
      insertedDocs[4].slice(),
      insertedDocs[5].slice(),
      insertedDocs[6].slice(),
      insertedDocs[7].slice(),
      insertedDocs[8].slice(),
      insertedDocs[9].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[13].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[19].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[22].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value']) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (bool)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'type', 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (bool) with bound params
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, @type, 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d",
      arangodb::velocypack::Parser::fromJson("{ \"type\" : \"type\" }")
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (bool) with bound view name
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW @@testView FILTER EXISTS(d.value, @type, 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d",
      arangodb::velocypack::Parser::fromJson("{ \"type\" : \"type\", \"@testView\": \"testView\" }")
    );

    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (bool) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'bool') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (boolean)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'type', 'boolean') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (boolean) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[1].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'boolean') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (numeric)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[3].slice(),
      insertedDocs[6].slice(),
      insertedDocs[7].slice(),
      insertedDocs[8].slice(),
      insertedDocs[9].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[13].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[19].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[22].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'type', 'numeric') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (numeric) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[3].slice(),
      insertedDocs[6].slice(),
      insertedDocs[7].slice(),
      insertedDocs[8].slice(),
      insertedDocs[9].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[13].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[19].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[22].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'numeric') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (numeric) via [], limit  5
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[3].slice(),
      insertedDocs[6].slice(),
      insertedDocs[7].slice(),
      insertedDocs[8].slice(),
      insertedDocs[9].slice()
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'numeric') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq LIMIT 5 RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (null)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[0].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'type', 'null') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (null) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[0].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'null') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (string)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[2].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'type', 'string') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (string) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[2].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'type', 'string') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (analyzer)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[2].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value, 'analyzer', 'identity') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (analyzer) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[2].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'], 'analyzer', 'identity') SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (array)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[4].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value[1]) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (array) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[4].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value'][1]) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (object)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[5].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d.value.b) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // test existent (object) via []
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[5].slice(),
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN VIEW testView FILTER EXISTS(d['value']['b']) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------