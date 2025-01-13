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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////
#include "AsyncRegistryServer/Stacktrace/forest.h"
#include "AsyncRegistryServer/Stacktrace/depth_first.h"

#include <gtest/gtest.h>

using namespace arangodb::async_registry;

TEST(AsyncRegistryStacktraceTest, insert_nodes_into_forest) {
  Forest<std::string> forest;

  forest.insert((void*)32, (void*)1, "first");
  forest.insert((void*)4, (void*)32, "second");
  forest.insert((void*)8, (void*)1, "third");
  forest.insert((void*)4, (void*)2, "second_overwritten");

  ASSERT_EQ(forest, (Forest<std::string>{
                        {(void*)1, (void*)32, (void*)1},
                        {"first", "second", "third"},
                        {{(void*)32, 0}, {(void*)4, 1}, {(void*)8, 2}}}));
  ASSERT_EQ(forest.node((void*)32), "first");
  ASSERT_EQ(forest.node((void*)4), "second");
  ASSERT_EQ(forest.node((void*)8), "third");
  ASSERT_EQ(forest.node((void*)1), std::nullopt);
}

TEST(AsyncRegistryStacktraceTest, index_forest) {
  Forest<std::string> forest;
  forest.insert((void*)1, (void*)2, "first");
  forest.insert((void*)2, (void*)4, "second");
  forest.insert((void*)3, (void*)2, "third");
  forest.insert((void*)4, (void*)32, "third");

  auto indexed = forest.index_by_awaitee();

  ASSERT_EQ(indexed.children((void*)1), std::vector<void*>{});
  ASSERT_EQ(indexed.children((void*)2),
            (std::vector<void*>{(void*)3, (void*)1}));  // order does not matter
  ASSERT_EQ(indexed.children((void*)3), std::vector<void*>{});
  ASSERT_EQ(indexed.children((void*)4), std::vector<void*>{(void*)2});
  ASSERT_EQ(indexed.children((void*)32),
            std::vector<void*>{});  // exists as waiter but not as proper node
  ASSERT_EQ(indexed.children((void*)8),
            std::vector<void*>{});  // node does not exist at all
  ASSERT_EQ(forest, (Forest<std::string>{{}, {}, {}}));
}

TEST(AsyncRegistryStacktraceTest, executes_post_ordered_depth_first) {
  Forest<std::string> forest;
  forest.insert((void*)1, (void*)0, "root");
  forest.insert((void*)2, (void*)1, "node");
  forest.insert((void*)3, (void*)2, "node");
  forest.insert((void*)4, (void*)2, "node");
  forest.insert((void*)5, (void*)3, "leaf");
  forest.insert((void*)6, (void*)3, "leaf");
  forest.insert((void*)7, (void*)4, "leaf");
  forest.insert((void*)8, (void*)32, "leaf");
  auto indexed = forest.index_by_awaitee();

  auto dfs = DFS_PostOrder{indexed, (void*)1};

  ASSERT_EQ(dfs.next(), std::make_pair((void*)5, (size_t)3));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)6, (size_t)3));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)3, (size_t)2));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)7, (size_t)3));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)4, (size_t)2));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)2, (size_t)1));
  ASSERT_EQ(dfs.next(), std::make_pair((void*)1, (size_t)0));
  ASSERT_EQ(dfs.next(), std::nullopt);

  auto dfs_of_another_tree = DFS_PostOrder{indexed, (void*)32};
  ASSERT_EQ(dfs_of_another_tree.next(), std::make_pair((void*)32, (size_t)0));
  ASSERT_EQ(dfs_of_another_tree.next(), std::nullopt);
  // does not also give 8 back because 32 is not a proper node in the forest

  auto dfs_of_unexistend_node = DFS_PostOrder{indexed, (void*)10};
  ASSERT_EQ(dfs_of_unexistend_node.next(),
            std::make_pair((void*)10, (size_t)0));
  ASSERT_EQ(dfs_of_unexistend_node.next(), std::nullopt);
}
