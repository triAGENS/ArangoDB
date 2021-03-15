////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "fakeit.hpp"

#include "Aql/AqlValue.h"
#include "Aql/Query.h"
#include "Aql/QueryWarnings.h"
#include "Cluster/ServerState.h"
#include "Graph/Cache/RefactoredClusterTraverserCache.h"
#include "Graph/ClusterTraverserCache.h"
#include "Graph/GraphTestTools.h"
#include "Graph/TraverserOptions.h"
#include "Transaction/Methods.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::graph;

namespace arangodb {
namespace tests {
namespace cluster_traverser_cache_test {

class ClusterTraverserCacheTest : public ::testing::Test {
 protected:
  ServerState::RoleEnum oldRole;

  graph::GraphTestSetup s;
  graph::MockGraphDatabase gdb;

  ClusterTraverserCacheTest() : gdb(s.server, "testVocbase") {}

  ~ClusterTraverserCacheTest() = default;
};

TEST_F(ClusterTraverserCacheTest, it_should_return_a_null_aqlvalue_if_vertex_not_cached) {
  std::unordered_map<ServerID, aql::EngineId> engines;
  std::string vertexId = "UnitTest/Vertex";
  std::string expectedMessage = "vertex '" + vertexId + "' not found";

  auto q = gdb.getQuery("RETURN 1", std::vector<std::string>{});

  traverser::TraverserOptions opts{*q};
  ClusterTraverserCache testee(*q, &engines, &opts);

  // NOTE: we do not put anything into the cache, so we get null for any vertex
  AqlValue val;
  testee.appendVertex(arangodb::velocypack::StringRef(vertexId), val);
  ASSERT_TRUE(val.isNull(false));
  auto all = q->warnings().all();
  ASSERT_TRUE(all.size() == 1);
  ASSERT_TRUE(all[0].first == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
  ASSERT_TRUE(all[0].second == expectedMessage);
}

TEST_F(ClusterTraverserCacheTest, it_should_insert_a_null_vpack_if_vertex_not_cached) {
  std::unordered_map<ServerID, aql::EngineId> engines;
  std::string vertexId = "UnitTest/Vertex";
  std::string expectedMessage = "vertex '" + vertexId + "' not found";

  auto q = gdb.getQuery("RETURN 1", std::vector<std::string>{});
  VPackBuilder result;
  traverser::TraverserOptions opts{*q};
  ClusterTraverserCache testee(*q, &engines, &opts);

  // NOTE: we do not put anything into the cache, so we get null for any vertex
  testee.appendVertex(arangodb::velocypack::StringRef(vertexId), result);

  VPackSlice sl = result.slice();
  ASSERT_TRUE(sl.isNull());

  auto all = q->warnings().all();
  ASSERT_TRUE(all.size() == 1);
  ASSERT_TRUE(all[0].first == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
  ASSERT_TRUE(all[0].second == expectedMessage);
}

class RefactoredClusterTraverserCacheTest : public ::testing::Test {
 protected:
  arangodb::ResourceMonitor _monitor{};
  std::unique_ptr<arangodb::graph::RefactoredClusterTraverserCache> _cache =
      std::make_unique<arangodb::graph::RefactoredClusterTraverserCache>(_monitor);

  RefactoredClusterTraverserCacheTest() {}

  ~RefactoredClusterTraverserCacheTest() = default;

  arangodb::graph::RefactoredClusterTraverserCache& cache() { return *_cache; }

  void TearDown() {
    // After every test ensure that the ResourceMonitor is conting down to 0 again
    _cache.reset();
    EXPECT_EQ(_monitor.currentMemoryUsage(), 0)
        << "Resource Monitor is not reset to 0 after deletion of the cache.";
  }

  void expectIsNotCached(VertexType const& vertexId) {
    EXPECT_FALSE(cache().isVertexCached(vertexId));
    auto result = cache().getCachedVertex(vertexId);
    EXPECT_TRUE(result.isNull());
  }
};

TEST_F(RefactoredClusterTraverserCacheTest, cache_a_single_vertex) {
  auto data = VPackParser::fromJson(R"({"_key":"123", "value":123})");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc.get("_key")};
  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();
  expectIsNotCached(key);
  testee.cacheVertex(key, doc);

  EXPECT_TRUE(testee.isVertexCached(key));
  EXPECT_LT(resourceBefore, _monitor.currentMemoryUsage())
      << "Did not increase memory usage.";
  {
    auto result = testee.getCachedVertex(key);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc, true));
  }
}

TEST_F(RefactoredClusterTraverserCacheTest, cache_multiple_vertices) {
  auto data = VPackParser::fromJson(R"({"_key":"123", "value":123})");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc.get("_key")};

  auto data2 = VPackParser::fromJson(R"({"_key":"456", "value":456})");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2.get("_key")};
  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();
  expectIsNotCached(key);
  expectIsNotCached(key2);

  testee.cacheVertex(key, doc);

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  testee.cacheVertex(key2, doc2);

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "Did not increase memory usage.";

  EXPECT_TRUE(testee.isVertexCached(key));
  {
    auto result = testee.getCachedVertex(key);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc, true));
  }

  EXPECT_TRUE(testee.isVertexCached(key2));
  {
    auto result = testee.getCachedVertex(key2);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc2, true));
  }
}

TEST_F(RefactoredClusterTraverserCacheTest, cache_same_vertex_twice) {
  auto data = VPackParser::fromJson(R"({"_key":"123", "value":123})");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc.get("_key")};

  // We simulate that we get the same Document data from two sources.
  // To make sure we keep the first copy, we try to insert a different value for the same _key
  // This will not happen in production, just to varify results here.
  auto data2 = VPackParser::fromJson(R"({"_key":"123", "value":456})");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2.get("_key")};

  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();
  expectIsNotCached(key);
  expectIsNotCached(key2);

  testee.cacheVertex(key, doc);

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  testee.cacheVertex(key2, doc2);

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_EQ(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "Did count the same vertex twice";

  EXPECT_TRUE(testee.isVertexCached(key));
  {
    auto result = testee.getCachedVertex(key);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc, true));
  }

  EXPECT_TRUE(testee.isVertexCached(key2));
  {
    auto result = testee.getCachedVertex(key2);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc, true));
  }
}

TEST_F(RefactoredClusterTraverserCacheTest, cache_same_vertex_twice_after_clear) {
  auto data = VPackParser::fromJson(R"({"_key":"123", "value":123})");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc.get("_key")};

  // We simulate that we get the same Document data from two sources.
  // To make sure we keep the first copy, we try to insert a different value for the same _key
  // This will not happen in production, just to varify results here.
  auto data2 = VPackParser::fromJson(R"({"_key":"123", "value":456})");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2.get("_key")};

  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();
  expectIsNotCached(key);
  expectIsNotCached(key2);

  testee.cacheVertex(key, doc);

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  testee.clear();

  // Test everything is empty.
  expectIsNotCached(key);
  expectIsNotCached(key2);
  EXPECT_EQ(resourceBefore, _monitor.currentMemoryUsage())
      << "Did not reset resource monitor.";

  testee.cacheVertex(key2, doc2);

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterSecondInsert)
      << "Did not increase memory usage.";
  EXPECT_EQ(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "Did count different counts";

  EXPECT_TRUE(testee.isVertexCached(key2));
  {
    auto result = testee.getCachedVertex(key2);
    EXPECT_FALSE(result.isNull());
    EXPECT_TRUE(basics::VelocyPackHelper::equal(result, doc2, true));
  }
}

TEST_F(RefactoredClusterTraverserCacheTest, persist_single_string) {
  auto data = VPackParser::fromJson(R"("123")");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc};
  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();

  auto persisted = testee.persistString(key);
  EXPECT_TRUE(key.equals(persisted));
  EXPECT_NE(key.begin(), persisted.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";
  EXPECT_LT(resourceBefore, _monitor.currentMemoryUsage())
      << "Did not increase memory usage.";
}

TEST_F(RefactoredClusterTraverserCacheTest, persist_multiple_strings) {
  auto data = VPackParser::fromJson(R"("123")");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc};

  auto data2 = VPackParser::fromJson(R"("456")");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2};
  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();

  auto persisted = testee.persistString(key);
  EXPECT_TRUE(key.equals(persisted));
  EXPECT_NE(key.begin(), persisted.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  auto persisted2 = testee.persistString(key2);
  EXPECT_TRUE(key2.equals(persisted2));
  EXPECT_NE(key2.begin(), persisted2.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "Did not increase memory usage.";
  EXPECT_NE(persisted.begin(), persisted2.begin())
      << "Cannot hand out the same address twice";
}

TEST_F(RefactoredClusterTraverserCacheTest, persist_same_string_twice) {
  auto data = VPackParser::fromJson(R"("123")");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc};

  auto data2 = VPackParser::fromJson(R"("123")");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2};
  {
    // Requirements for the test
    ASSERT_TRUE(key.equals(key2)) << "Keys do not have same content.";
    ASSERT_NE(key.begin(), key2.begin()) << "Keys do have the same pointer.";
  }

  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();

  auto persisted = testee.persistString(key);
  EXPECT_TRUE(key.equals(persisted));
  EXPECT_NE(key.begin(), persisted.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  auto persisted2 = testee.persistString(key2);
  EXPECT_TRUE(key2.equals(persisted2));
  EXPECT_NE(key2.begin(), persisted2.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";
  EXPECT_EQ(persisted.begin(), persisted2.begin())
      << "We should only cache the same value once.";

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_EQ(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "We counted the same string ref multiple times.";
}

TEST_F(RefactoredClusterTraverserCacheTest, persist_same_string_twice_after_clear) {
  auto data = VPackParser::fromJson(R"("123")");
  VPackSlice doc = data->slice();
  HashedStringRef key{doc};

  auto data2 = VPackParser::fromJson(R"("123")");
  VPackSlice doc2 = data2->slice();
  HashedStringRef key2{doc2};
  {
    // Requirements for the test
    ASSERT_TRUE(key.equals(key2)) << "Keys do not have same content.";
    ASSERT_NE(key.begin(), key2.begin()) << "Keys do have the same pointer.";
  }

  auto& testee = cache();
  auto resourceBefore = _monitor.currentMemoryUsage();

  auto persisted = testee.persistString(key);
  EXPECT_TRUE(key.equals(persisted));
  EXPECT_NE(key.begin(), persisted.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";

  auto resourceAfterFirstInsert = _monitor.currentMemoryUsage();
  EXPECT_LT(resourceBefore, resourceAfterFirstInsert)
      << "Did not increase memory usage.";

  testee.clear();

  EXPECT_EQ(resourceBefore, _monitor.currentMemoryUsage())
      << "Did not reset resource monitor.";

  auto persisted2 = testee.persistString(key2);
  EXPECT_TRUE(key2.equals(persisted2));
  EXPECT_NE(key2.begin(), persisted2.begin())
      << "We do not have different char pointer. The persisted one needs to be "
         "internally managed";

  auto resourceAfterSecondInsert = _monitor.currentMemoryUsage();
  EXPECT_EQ(resourceAfterFirstInsert, resourceAfterSecondInsert)
      << "Persisting of the same key has different costs.";
}

}  // namespace cluster_traverser_cache_test
}  // namespace tests
}  // namespace arangodb
