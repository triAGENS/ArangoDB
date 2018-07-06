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

#include "utils/misc.hpp"
#include "utils/string.hpp"
#include "utils/thread_utils.hpp"
#include "utils/version_defines.hpp"

#include "Aql/AqlFunctionFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/Containers.h"
#include "IResearch/IResearchFeature.h"
#include "Rest/Version.h"
#include "StorageEngine/EngineSelectorFeature.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchFeatureSetup {
  StorageEngineMock engine;

  IResearchFeatureSetup() {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init();
  }

  ~IResearchFeatureSetup() {
    arangodb::EngineSelectorFeature::ENGINE = nullptr;
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchFeatureTest", "[iresearch][iresearch-feature]") {
  IResearchFeatureSetup s;
  UNUSED(s);

SECTION("test_start") {
  arangodb::application_features::ApplicationServer server(nullptr, nullptr);
  auto* functions = new arangodb::aql::AqlFunctionFeature(&server);
  arangodb::iresearch::IResearchFeature iresearch(&server);
  auto cleanup = irs::make_finally([functions]()->void{ functions->unprepare(); });

  enum class FunctionType {
    FILTER = 0,
    SCORER
  };

  std::map<irs::string_ref, std::pair<irs::string_ref, FunctionType>> expected = {
    // filter functions
    { "EXISTS", { ".|.,.", FunctionType::FILTER } },
    { "PHRASE", { ".,.|.+", FunctionType::FILTER } },
    { "STARTS_WITH", { ".,.|.", FunctionType::FILTER } },
    { "MIN_MATCH", { ".,.|.+", FunctionType::FILTER } },

    // context functions
    { "ANALYZER", { ".,.", FunctionType::FILTER } },
    { "BOOST", { ".,.", FunctionType::FILTER } },

    // scorer functions
    { "BM25", { ".|+", FunctionType::SCORER } },
    { "TFIDF", { ".|+", FunctionType::SCORER } },
  };

  server.addFeature(functions);
  functions->prepare();

  for(auto& entry: expected) {
    auto* function = arangodb::iresearch::getFunction(*functions, entry.first);
    CHECK((nullptr == function));
  };

  iresearch.start();

  for(auto& entry: expected) {
    auto* function = arangodb::iresearch::getFunction(*functions, entry.first);
    CHECK((nullptr != function));
    CHECK((entry.second.first == function->arguments));
    CHECK((
      entry.second.second == FunctionType::FILTER && arangodb::iresearch::isFilter(*function)
      || entry.second.second == FunctionType::SCORER && arangodb::iresearch::isScorer(*function)
    ));
  };
}

SECTION("IResearch_version") {
  CHECK(IResearch_version == arangodb::rest::Version::getIResearchVersion());
  CHECK(IResearch_version == arangodb::rest::Version::Values["iresearch-version"]);
}

SECTION("test_async") {
  struct DestructFlag {
    bool* _flag;
    DestructFlag(bool &flag): _flag(&flag) {}
    ~DestructFlag() { if (_flag) *_flag = true; }
    DestructFlag(DestructFlag const& other): _flag(other._flag) { }
    DestructFlag(DestructFlag&& other) noexcept: _flag(other._flag) { other._flag = nullptr; }
    DestructFlag& operator=(DestructFlag const& other) { _flag = other._flag; return *this; }
    DestructFlag& operator=(DestructFlag&& other) noexcept { _flag = other._flag; other._flag = nullptr; return *this; }
  };

  // schedule task (null resource mutex)
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(nullptr, 1, [&cond, &mutex, flag](size_t&, bool)->bool { SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(100))));
    std::this_thread::yield();
    CHECK((true == deallocated));
  }

  // schedule task (null resource mutex value)
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(nullptr);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 1, [&cond, &mutex, flag](size_t&, bool)->bool { SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout == cond.wait_for(lock, std::chrono::milliseconds(100))));
    std::this_thread::yield();
    CHECK((true == deallocated));
  }

  // schedule task (null functr)
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 1, {});
    CHECK((std::cv_status::timeout == cond.wait_for(lock, std::chrono::milliseconds(100))));
    resourceMutex->reset(); // should not deadlock if task released
  }

  // schedule task (wait indefinite)
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    size_t count = 0;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(nullptr, 0, [&cond, &mutex, flag, &count](size_t&, bool)->bool { ++count; SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout == cond.wait_for(lock, std::chrono::milliseconds(100))));
    CHECK((false == deallocated)); // still scheduled
    CHECK((0 == count));
  }

  // single-run task
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 1, [&cond, &mutex, flag](size_t&, bool)->bool { SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(100))));
    std::this_thread::yield();
    CHECK((true == deallocated));
  }

  // multi-run task
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    size_t count = 0;
    auto last = std::chrono::system_clock::now();
    std::chrono::system_clock::duration diff;
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 1, [&cond, &mutex, flag, &count, &last, &diff](size_t& timeoutMsec, bool)->bool {
      diff = std::chrono::system_clock::now() - last;
      last = std::chrono::system_clock::now();
      timeoutMsec = 100;
      if (++count <= 1) return true;
      SCOPED_LOCK(mutex);
      cond.notify_all();
      return false;
    });
    CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(1000))));
    std::this_thread::yield();
    CHECK((true == deallocated));
    CHECK((2 == count));
    CHECK((std::chrono::milliseconds(100) < diff));
  }

  // trigger task by notify
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    bool deallocated = false;
    bool execVal = true;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    auto last = std::chrono::system_clock::now();
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 1000, [&cond, &mutex, flag, &execVal](size_t&, bool exec)->bool { execVal = exec; SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout == cond.wait_for(lock, std::chrono::milliseconds(100))));
    CHECK((false == deallocated));
    feature.asyncNotify();
    CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(100))));
    std::this_thread::yield();
    CHECK((true == deallocated));
    CHECK((false == execVal));
    auto diff = std::chrono::system_clock::now() - last;
    CHECK((std::chrono::milliseconds(1000) > diff));
  }

  // trigger by timeout
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    arangodb::iresearch::IResearchFeature feature(&server);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    bool deallocated = false;
    bool execVal = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    auto last = std::chrono::system_clock::now();
    SCOPED_LOCK_NAMED(mutex, lock);

    feature.async(resourceMutex, 100, [&cond, &mutex, flag, &execVal](size_t&, bool exec)->bool { execVal = exec; SCOPED_LOCK(mutex); cond.notify_all(); return false; });
    CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(1000))));
    std::this_thread::yield();
    CHECK((true == deallocated));
    CHECK((true == execVal));
    auto diff = std::chrono::system_clock::now() - last;
    CHECK((std::chrono::milliseconds(200) >= diff));
  }

  // deallocate empty
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);

    {
      arangodb::iresearch::IResearchFeature feature(&server);
    }
  }

  // deallocate with running tasks
  {
    arangodb::application_features::ApplicationServer server(nullptr, nullptr);
    auto resourceMutex = std::make_shared<arangodb::iresearch::ResourceMutex>(&server);
    bool deallocated = false;
    DestructFlag flag(deallocated);
    std::condition_variable cond;
    std::mutex mutex;
    SCOPED_LOCK_NAMED(mutex, lock);

    {
      arangodb::iresearch::IResearchFeature feature(&server);

      feature.async(resourceMutex, 1, [&cond, &mutex, flag](size_t& timeoutMsec, bool)->bool { SCOPED_LOCK(mutex); cond.notify_all(); timeoutMsec = 100; return true; });
      CHECK((std::cv_status::timeout != cond.wait_for(lock, std::chrono::milliseconds(100))));
    }

    CHECK((true == deallocated));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generate tests
////////////////////////////////////////////////////////////////////////////////

}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------