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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <random>
#include <vector>

#include "gtest/gtest.h"

#include "Basics/hashes.h"
#include "Containers/MerkleTree.h"
#include "Logger/LogMacros.h"

namespace {
std::vector<std::uint64_t> permutation(std::uint64_t n) {
  std::vector<std::uint64_t> v;

  v.reserve(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    v.emplace_back(i);
  }

  std::random_device rd;
  std::mt19937_64 g(rd());
  std::shuffle(v.begin(), v.end(), g);

  return v;
}

bool diffAsExpected(
    ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>& t1,
    ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>& t2,
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& expected) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> d1 = t1.diff(t2);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> d2 = t2.diff(t1);

  auto printTrees = [&t1, &t2]() -> void {
    LOG_DEVEL << "T1: " << t1;
    LOG_DEVEL << "T2: " << t2;
  };

  if (d1.size() != expected.size() || d2.size() != expected.size()) {
    printTrees();
    return false;
  }
  for (std::uint64_t i = 0; i < expected.size(); ++i) {
    if (d1[i].first != expected[i].first || d1[i].second != expected[i].second ||
        d2[i].first != expected[i].first || d2[i].second != expected[i].second) {
      printTrees();
      return false;
    }
  }
  return true;
}

bool partitionAsExpected(
    ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>& tree,
    std::uint64_t count, std::vector<std::pair<std::uint64_t, std::uint64_t>> expected) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> partitions =
      tree.partitionKeys(count);

  if (partitions.size() != expected.size()) {
    return false;
  }
  for (std::uint64_t i = 0; i < expected.size(); ++i) {
    if (partitions[i].first != expected[i].first ||
        partitions[i].second != expected[i].second) {
      return false;
    }
  }
  return true;
}
}  // namespace

class TestNodeCountAtDepth
    : public arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> {
  static_assert(nodeCountAtDepth(0) == 1);
  static_assert(nodeCountAtDepth(1) == 8);
  static_assert(nodeCountAtDepth(2) == 64);
  static_assert(nodeCountAtDepth(3) == 512);
  // ...
  static_assert(nodeCountAtDepth(10) == 1073741824);
};

class InternalMerkleTreeTest
    : public ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>,
      public ::testing::Test {
 public:
  InternalMerkleTreeTest()
      : MerkleTree<::arangodb::containers::FnvHashProvider, 3>(2, 0, 64) {}
};

TEST_F(InternalMerkleTreeTest, test_childrenAreLeaves) {
  ASSERT_FALSE(childrenAreLeaves(0));
  for (std::uint64_t index = 1; index < 9; ++index) {
    ASSERT_TRUE(childrenAreLeaves(index));
  }
  for (std::uint64_t index = 9; index < 73; ++index) {
    ASSERT_FALSE(childrenAreLeaves(index));
  }
}

TEST_F(InternalMerkleTreeTest, test_chunkRange) {
  auto r = chunkRange(0, 0);
  ASSERT_EQ(r.first, 0);
  ASSERT_EQ(r.second, 63);

  for (std::uint64_t chunk = 0; chunk < 8; ++chunk) {
    auto r = chunkRange(chunk, 1);
    ASSERT_EQ(r.first, chunk * 8);
    ASSERT_EQ(r.second, ((chunk + 1) * 8) - 1);
  }

  for (std::uint64_t chunk = 0; chunk < 64; ++chunk) {
    auto r = chunkRange(chunk, 2);
    ASSERT_EQ(r.first, chunk);
    ASSERT_EQ(r.second, chunk);
  }
}

TEST_F(InternalMerkleTreeTest, test_index) {
  std::pair<std::uint64_t, std::uint64_t> range = this->range();
  ASSERT_EQ(range.first, 0);
  ASSERT_EQ(range.second, 64);

  // make sure depth 0 always gets us 0
  ASSERT_EQ(index(0, 0), 0);
  ASSERT_EQ(index(63, 0), 0);

  // check boundaries at level 1
  for (std::uint64_t chunk = 0; chunk < 8; ++chunk) {
    std::uint64_t left = chunk * 8;
    std::uint64_t right = ((chunk + 1) * 8) - 1;
    ASSERT_EQ(index(left, 1), chunk + 1);
    ASSERT_EQ(index(right, 1), chunk + 1);
  }

  // check boundaries at level 2
  for (std::uint64_t chunk = 0; chunk < 64; ++chunk) {
    std::uint64_t left = chunk;  // only one value per chunk
    ASSERT_EQ(index(left, 2), chunk + 9);
  }
}

TEST_F(InternalMerkleTreeTest, test_modify) {
  std::pair<std::uint64_t, std::uint64_t> range = this->range();
  ASSERT_EQ(range.first, 0);
  ASSERT_EQ(range.second, 64);

  ASSERT_EQ(count(), 0);
  // check that an attempt to remove will fail if it's empty
  ASSERT_THROW(modify(0, false), std::invalid_argument);
  ASSERT_EQ(count(), 0);

  // insert a single value
  modify(0, true);
  ASSERT_EQ(count(), 1);

  // build set of indexes that should be touched
  std::set<std::uint64_t> indices0;
  for (std::uint64_t depth = 0; depth <= meta().maxDepth; ++depth) {
    indices0.emplace(this->index(0, depth));
  }

  ::arangodb::containers::FnvHashProvider hasher;
  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    bool inSet0 = indices0.find(index) != indices0.end();
    std::uint64_t expectedCount = inSet0 ? 1 : 0;
    std::uint64_t expectedHash = inSet0 ? hasher.hash(0) : 0;

    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, expectedCount);
    ASSERT_EQ(node.hash, expectedHash);
  }

  // insert another value, minimal overlap
  modify(63, true);
  ASSERT_EQ(count(), 2);

  // build set of indexes that should be touched
  std::set<std::uint64_t> indices63;
  for (std::uint64_t depth = 0; depth <= meta().maxDepth; ++depth) {
    indices63.emplace(this->index(63, depth));
  }

  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    bool inSet0 = indices0.find(index) != indices0.end();
    std::uint64_t expectedCount = inSet0 ? 1 : 0;
    std::uint64_t expectedHash = inSet0 ? hasher.hash(0) : 0;
    if (indices63.find(index) != indices63.end()) {
      ++expectedCount;
      expectedHash ^= hasher.hash(63);
    }

    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, expectedCount);
    ASSERT_EQ(node.hash, expectedHash);
  }

  // insert another value, more overlap
  modify(1, true);
  ASSERT_EQ(count(), 3);

  // build set of indexes that should be touched
  std::set<std::uint64_t> indices1;
  for (std::uint64_t depth = 0; depth <= meta().maxDepth; ++depth) {
    indices1.emplace(this->index(1, depth));
  }

  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    bool inSet0 = indices0.find(index) != indices0.end();
    std::uint64_t expectedCount = inSet0 ? 1 : 0;
    std::uint64_t expectedHash = inSet0 ? hasher.hash(0) : 0;
    if (indices1.find(index) != indices1.end()) {
      ++expectedCount;
      expectedHash ^= hasher.hash(1);
    }
    if (indices63.find(index) != indices63.end()) {
      ++expectedCount;
      expectedHash ^= hasher.hash(63);
    }

    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, expectedCount);
    ASSERT_EQ(node.hash, expectedHash);
  }

  // remove a value, minimal overlap
  modify(63, false);
  ASSERT_EQ(count(), 2);

  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    bool inSet0 = indices0.find(index) != indices0.end();
    std::uint64_t expectedCount = inSet0 ? 1 : 0;
    std::uint64_t expectedHash = inSet0 ? hasher.hash(0) : 0;
    if (indices1.find(index) != indices1.end()) {
      ++expectedCount;
      expectedHash ^= hasher.hash(1);
    }

    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, expectedCount);
    ASSERT_EQ(node.hash, expectedHash);
  }

  // remove a value, maximal overlap
  modify(1, false);
  ASSERT_EQ(count(), 1);

  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    bool inSet0 = indices0.find(index) != indices0.end();
    std::uint64_t expectedCount = inSet0 ? 1 : 0;
    std::uint64_t expectedHash = inSet0 ? hasher.hash(0) : 0;

    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, expectedCount);
    ASSERT_EQ(node.hash, expectedHash);
  }

  // remove a value, maximal overlap
  modify(0, false);
  ASSERT_EQ(count(), 0);

  // check that it sets everything it should, and nothing it shouldn't
  for (std::uint64_t index = 0; index < 73; ++index) {
    InternalMerkleTreeTest::Node& node = this->node(index);
    ASSERT_EQ(node.count, 0);
    ASSERT_EQ(node.hash, 0);
  }
}

TEST_F(InternalMerkleTreeTest, test_grow) {
  std::pair<std::uint64_t, std::uint64_t> range = this->range();
  ASSERT_EQ(range.first, 0);
  ASSERT_EQ(range.second, 64);

  // fill the tree, but not enough that it grows
  for (std::uint64_t i = 0; i < 64; ++i) {
    insert(i);
  }
  range = this->range();
  ASSERT_EQ(range.first, 0);
  ASSERT_EQ(range.second, 64);

  ::arangodb::containers::FnvHashProvider hasher;
  // check that tree state is as expected prior to growing
  {
    std::uint64_t hash0 = 0;
    std::uint64_t hash1[8] = {0};
    std::uint64_t hash2[64] = {0};
    for (std::uint64_t i = 0; i < 64; ++i) {
      hash0 ^= hasher.hash(static_cast<std::uint64_t>(i));
      hash1[i / 8] ^= hasher.hash(static_cast<std::uint64_t>(i));
      hash2[i] ^= hasher.hash(static_cast<std::uint64_t>(i));
    }
    for (std::uint64_t i = 0; i < 64; ++i) {
      Node& node = this->node(this->index(static_cast<std::uint64_t>(i), 2));
      ASSERT_EQ(node.count, 1);
      ASSERT_EQ(node.hash, hash2[i]);
    }
    for (std::uint64_t i = 0; i < 8; ++i) {
      Node& node = this->node(i + 1);
      ASSERT_EQ(node.count, 8);
      ASSERT_EQ(node.hash, hash1[i]);
    }
    {
      Node& node = this->node(0);
      ASSERT_EQ(node.count, 64);
      ASSERT_EQ(node.hash, hash0);
    }
  }

  // insert some more and cause it to grow
  for (std::uint64_t i = 64; i < 128; ++i) {
    insert(i);
  }
  range = this->range();
  ASSERT_EQ(range.first, 0);
  ASSERT_EQ(range.second, 128);

  // check that tree state is as expected after growing
  {
    std::uint64_t hash0 = 0;
    std::uint64_t hash1[8] = {0};
    std::uint64_t hash2[64] = {0};
    for (std::uint64_t i = 0; i < 128; ++i) {
      hash0 ^= hasher.hash(static_cast<std::uint64_t>(i));
      hash1[i / 16] ^= hasher.hash(static_cast<std::uint64_t>(i));
      hash2[i / 2] ^= hasher.hash(static_cast<std::uint64_t>(i));
    }
    for (std::uint64_t i = 0; i < 64; ++i) {
      Node& node = this->node(i + 9);
      ASSERT_EQ(node.count, 2);
      if (node.hash != hash2[i]) {
      }
      ASSERT_EQ(node.hash, hash2[i]);
    }
    for (std::uint64_t i = 0; i < 8; ++i) {
      Node& node = this->node(i + 1);
      ASSERT_EQ(node.count, 16);
      ASSERT_EQ(node.hash, hash1[i]);
    }
    {
      Node& node = this->node(0);
      ASSERT_EQ(node.count, 128);
      ASSERT_EQ(node.hash, hash0);
    }
  }
}

TEST_F(InternalMerkleTreeTest, test_partition) {
  ASSERT_TRUE(::partitionAsExpected(*this, 0, {{0, 64}}));

  for (std::uint64_t i = 0; i < 32; ++i) {
    this->insert(2 * i);
  }

  ASSERT_TRUE(::partitionAsExpected(*this, 0, {{0, 64}}));
  ASSERT_TRUE(::partitionAsExpected(*this, 1, {{0, 64}}));
  ASSERT_TRUE(::partitionAsExpected(*this, 2, {{0, 30}, {31, 63}}));
  ASSERT_TRUE(::partitionAsExpected(*this, 3, {{0, 18}, {19, 40}, {41, 63}}));
  ASSERT_TRUE(::partitionAsExpected(*this, 4, {{0, 14}, {15, 30}, {31, 46}, {47, 63}}));
  ///
  ASSERT_TRUE(::partitionAsExpected(
      *this, 42,
      {{0, 0},   {1, 2},   {3, 4},   {5, 6},   {7, 8},   {9, 10},  {11, 12},
       {13, 14}, {15, 16}, {17, 18}, {19, 20}, {21, 22}, {23, 24}, {25, 26},
       {27, 28}, {29, 30}, {31, 32}, {33, 34}, {35, 36}, {37, 38}, {39, 40},
       {41, 42}, {43, 44}, {45, 46}, {47, 48}, {49, 50}, {51, 52}, {53, 54},
       {55, 56}, {57, 58}, {59, 60}, {61, 62}}));

  // now let's make the distribution more uneven and see how things go
  this->growRight(511);

  ASSERT_TRUE(::partitionAsExpected(*this, 3, {{0, 23}, {24, 47}, {48, 511}}));
  ASSERT_TRUE(::partitionAsExpected(*this, 4, {{0, 15}, {16, 31}, {32, 47}, {48, 511}}));

  // lump it all in one cell
  this->growRight(4095);

  ASSERT_TRUE(::partitionAsExpected(*this, 4, {{0, 63}}));
}

TEST(MerkleTreeTest, test_diff_equal) {
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, 0, 64);
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t2(2, 0, 64);

  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;  // empty
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  std::vector<std::uint64_t> order = ::permutation(64);
  for (std::uint64_t i : order) {
    t1.insert(i);
    t2.insert(i);
    ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  }

  order = ::permutation(64);
  for (std::uint64_t i : order) {
    t1.remove(i);
    t2.remove(i);
    ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  }
}

TEST(MerkleTreeTest, test_diff_one_empty) {
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, 0, 64);
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t2(2, 0, 64);

  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  for (std::uint64_t i = 0; i < 8; ++i) {
    t1.insert(8 * i);
    expected.emplace_back(std::make_pair(8 * i, 8 * i));
    ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  }

  expected.clear();
  for (std::uint64_t i = 0; i < 8; ++i) {
    t1.insert((8 * i) + 1);
    expected.emplace_back(std::make_pair(8 * i, (8 * i) + 1));
  }
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  expected.clear();
  for (std::uint64_t i = 0; i < 8; ++i) {
    t1.insert((8 * i) + 2);
    t1.insert((8 * i) + 3);
    expected.emplace_back(std::make_pair(8 * i, (8 * i) + 3));
  }
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  expected.clear();
  for (std::uint64_t i = 0; i < 8; ++i) {
    t1.insert((8 * i) + 4);
    t1.insert((8 * i) + 5);
    t1.insert((8 * i) + 6);
    t1.insert((8 * i) + 7);
  }
  expected.emplace_back(std::make_pair(0, 63));
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
}

TEST(MerkleTreeTest, test_diff_misc) {
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, 0, 64);
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t2(2, 0, 64);

  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  for (std::uint64_t i = 0; i < 32; ++i) {
    t1.insert(2 * i);
    expected.emplace_back(std::make_pair(2 * i, 2 * i));
  }
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  expected.clear();
  for (std::uint64_t i = 0; i < 32; ++i) {
    t2.insert((2 * i) + 1);
  }
  expected.emplace_back(std::make_pair(0, 63));
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  expected.clear();
  for (std::uint64_t i = 0; i < 16; ++i) {
    t1.insert((2 * i) + 1);
    expected.emplace_back(std::make_pair(2 * i, 2 * i));
  }
  expected.emplace_back(std::make_pair(32, 63));
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
}

TEST(MerkleTreeTest, test_serializeBinary) {
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, 0, 64);

  for (std::uint64_t i = 0; i < 32; ++i) {
    t1.insert(2 * i);
  }

  std::string t1s;
  t1.serializeBinary(t1s, true);
  std::unique_ptr<::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>> t2 =
      ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>::fromBuffer(t1s);
  ASSERT_NE(t2, nullptr);
  ASSERT_TRUE(t1.diff(*t2).empty());
  ASSERT_TRUE(t2->diff(t1).empty());
}

TEST(MerkleTreeTest, test_serializePortable) {
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, 0, 64);

  for (std::uint64_t i = 0; i < 32; ++i) {
    t1.insert(2 * i);
  }

  ::arangodb::velocypack::Builder t1s;
  t1.serialize(t1s);
  std::unique_ptr<::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>> t2 =
      ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>::deserialize(
          t1s.slice());
  ASSERT_NE(t2, nullptr);
  ASSERT_TRUE(t1.diff(*t2).empty());
  ASSERT_TRUE(t2->diff(t1).empty());
}

TEST(MerkleTreeTest, test_tree_based_on_2020_hlcs) {
  uint64_t rangeMin = uint64_t(1577836800000ULL) << 20ULL;
  uint64_t rangeMax = uint64_t(1654481800413577216ULL);
  
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> tree(6, rangeMin);
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  
  auto [left, right] = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);

  for (std::uint64_t i = rangeMin; i < rangeMin + 10000; ++i) {
    tree.insert(i);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(10000, tree.count());
  ASSERT_EQ(13533672744353677152ULL, tree.rootValue());
  std::tie(left, right) = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);
  
  for (std::uint64_t i = rangeMin; i < rangeMin + 10000; ++i) {
    tree.remove(i);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  std::tie(left, right) = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);

  // increase the pace
  constexpr std::uint64_t n = 10'000'000;
  constexpr std::uint64_t batchSize = 10'000;

  std::vector<std::uint64_t> revisions;
  revisions.reserve(batchSize);
  for (std::uint64_t i = rangeMin; i < rangeMin + n; i += batchSize) {
    revisions.clear();
    for (std::uint64_t j = 0; j < batchSize; ++j) {
      revisions.push_back(i + j);
    }
    tree.insert(revisions);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(10'000'000, tree.count());
  ASSERT_EQ(14528932675464142080ULL, tree.rootValue());
  std::tie(left, right) = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);
  
  for (std::uint64_t i = rangeMin; i < rangeMin + n; i += batchSize) {
    revisions.clear();
    for (std::uint64_t j = 0; j < batchSize; ++j) {
      revisions.push_back(i + j);
    }
    tree.remove(revisions);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  std::tie(left, right) = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);
}

TEST(MerkleTreeTest, test_large_steps) {
  uint64_t rangeMin = uint64_t(1577836800000ULL) << 20ULL;
  uint64_t rangeMax = uint64_t(1654481800413577216ULL);
  
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> tree(6, rangeMin);
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  
  auto [left, right] = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);

  constexpr std::uint64_t n = 100'000'000'000;
  constexpr std::uint64_t step = 10'000;

  for (std::uint64_t i = rangeMin; i < rangeMin + n; i += step) {
    tree.insert(i);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  EXPECT_EQ(10'000'000, tree.count());
  EXPECT_EQ(10681126656127731097ULL, tree.rootValue());
  std::tie(left, right) = tree.range();
  rangeMax = 1654481937835753472ULL;
  EXPECT_EQ(rangeMin, left);
  EXPECT_EQ(rangeMax, right);
  
  for (std::uint64_t i = rangeMin; i < rangeMin + n; i += step) {
    tree.remove(i);
  }
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  std::tie(left, right) = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);
}

TEST(MerkleTreeTest, test_check_consistency) {
  uint64_t rangeMin = uint64_t(1577836800000ULL) << 20ULL;
  uint64_t rangeMax = uint64_t(1654481800413577216ULL);
  
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> tree(6, rangeMin);
  
  ASSERT_EQ(6, tree.maxDepth());
  ASSERT_EQ(0, tree.count());
  ASSERT_EQ(0, tree.rootValue());
  
  // must not throw
  tree.checkConsistency();
  
  auto [left, right] = tree.range();
  ASSERT_EQ(rangeMin, left);
  ASSERT_EQ(rangeMax, right);

  constexpr std::uint64_t n = 100'000'000'000;
  constexpr std::uint64_t step = 10'000;

  for (std::uint64_t i = rangeMin; i < rangeMin + n; i += step) {
    tree.insert(i);
  }

  // must not throw
  tree.checkConsistency();
 
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  tree.corrupt(42, 23);

  // must throw
  try {
    tree.checkConsistency();
    ASSERT_TRUE(false);
  } catch (std::invalid_argument const&) {
  }
#endif
}

class MerkleTreeGrowTests
    : public ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3>,
      public ::testing::Test {
 public:
  MerkleTreeGrowTests()
      : MerkleTree<::arangodb::containers::FnvHashProvider, 3>(
          6, uint64_t(1577836800000ULL) << 20ULL) {}
};

TEST_F(MerkleTreeGrowTests, test_grow_left_simple) {
  uint64_t rangeMin = range().first;
  uint64_t initWidth = 1ULL << 24;
  uint64_t bucketWidth = 1ULL << 6;
  uint64_t rangeMax = range().second;
  ASSERT_EQ(rangeMin + initWidth, rangeMax);

  arangodb::containers::FnvHashProvider hasher;

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  
  insert(rangeMin);
  insert(rangeMin + bucketWidth);
  insert(rangeMin + 47 * bucketWidth);

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(3, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth), rootValue());

  // Now grow to the left:
  insert(rangeMin - 1);

  // Must not throw:
  checkConsistency();

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(4, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth) ^
            hasher.hash(rangeMin - 1), rootValue());
  ASSERT_EQ(rangeMin - initWidth, range().first);
  ASSERT_EQ(rangeMax, range().second);

  // Now check the bottommost buckets:
  Node& n = node(index(rangeMin, maxDepth()));
  ASSERT_EQ(2, n.count);
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth),
            n.hash);
  Node& n2 = node(index(rangeMin - 1, maxDepth()));
  ASSERT_EQ(1, n2.count);
  ASSERT_EQ(hasher.hash(rangeMin - 1), n2.hash);
  Node& n3 = node(index(rangeMin + 47 * bucketWidth, maxDepth()));
  ASSERT_EQ(1, n3.count);
  ASSERT_EQ(hasher.hash(rangeMin + 47 * bucketWidth), n3.hash);
}

TEST_F(MerkleTreeGrowTests, test_grow_left_with_shift) {
  uint64_t rangeMin = range().first;
  uint64_t initWidth = 1ULL << 24;
  uint64_t bucketWidth = 1ULL << 6;
  uint64_t rangeMax = range().second;
  ASSERT_EQ(rangeMin + initWidth, rangeMax);

  arangodb::containers::FnvHashProvider hasher;

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  
  // We grow once to the left, so that initialRangeMin - rangeMin is 2^24.
  // Then we grow to the right until the width is 2^(18+24) = 2^42.
  // The next grow operation after that needs to shift, since then
  // the size of a bucket becomes 2^24 and with the next grow operation
  // the difference initialRangeMin - rangeMin would no longer be divisble
  // by the bucket size.
  growLeft(rangeMin-1);
  for (int i = 0; i < 17; ++i) {
    growRight(rangeMax);
    rangeMax = range().second;
  }

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  ASSERT_EQ(rangeMin - initWidth, range().first);
  rangeMin = range().first;
  rangeMax = range().second;
  ASSERT_EQ(rangeMin + (1ULL << 42), rangeMax);
  bucketWidth = (range().second - range().first) >> 18;
  ASSERT_EQ(1ULL << 24, bucketWidth);

  insert(rangeMin);
  insert(rangeMin + bucketWidth);
  insert(rangeMin + 47 * bucketWidth);

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(3, count());
  ASSERT_EQ(rangeMin, range().first);
  ASSERT_EQ(rangeMax, range().second);
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth), rootValue());

  // Now grow to the left:
  insert(rangeMin - 1);

  // Must not throw:
  checkConsistency();

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(4, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth) ^
            hasher.hash(rangeMin - 1), rootValue());
  ASSERT_EQ(rangeMin - (rangeMax - rangeMin) + bucketWidth, range().first);
  ASSERT_EQ(rangeMax + bucketWidth, range().second);

  // Now check the bottommost buckets:
  Node& n = node(index(rangeMin, maxDepth()));
  ASSERT_EQ(2, n.count);
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin - 1), n.hash);
  Node& n2 = node(index(rangeMin + bucketWidth, maxDepth()));
  ASSERT_EQ(1, n2.count);
  ASSERT_EQ(hasher.hash(rangeMin + bucketWidth), n2.hash);
  Node& n3 = node(index(rangeMin + 47 * bucketWidth, maxDepth()));
  ASSERT_EQ(1, n3.count);
  ASSERT_EQ(hasher.hash(rangeMin + 47 * bucketWidth), n3.hash);
}

TEST_F(MerkleTreeGrowTests, test_grow_right_simple) {
  uint64_t rangeMin = range().first;
  uint64_t initWidth = 1ULL << 24;
  uint64_t bucketWidth = 1ULL << 6;
  uint64_t rangeMax = range().second;
  ASSERT_EQ(rangeMin + initWidth, rangeMax);

  arangodb::containers::FnvHashProvider hasher;

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  
  insert(rangeMin);
  insert(rangeMin + bucketWidth);
  insert(rangeMin + 47 * bucketWidth);

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(3, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth), rootValue());

  // Now grow to the right:
  insert(rangeMax + 42);

  // Must not throw:
  checkConsistency();

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(4, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth) ^
            hasher.hash(rangeMax + 42), rootValue());
  ASSERT_EQ(rangeMin, range().first);
  ASSERT_EQ(rangeMax + initWidth, range().second);

  // Now check the bottommost buckets:
  Node& n = node(index(rangeMin, maxDepth()));
  ASSERT_EQ(2, n.count);
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth),
            n.hash);
  Node& n2 = node(index(rangeMax + 42, maxDepth()));
  ASSERT_EQ(1, n2.count);
  ASSERT_EQ(hasher.hash(rangeMax + 42), n2.hash);
  Node& n3 = node(index(rangeMin + 47 * bucketWidth, maxDepth()));
  ASSERT_EQ(1, n3.count);
  ASSERT_EQ(hasher.hash(rangeMin + 47 * bucketWidth), n3.hash);
}

TEST_F(MerkleTreeGrowTests, test_grow_right_with_shift) {
  uint64_t rangeMin = range().first;
  uint64_t initWidth = 1ULL << 24;
  uint64_t bucketWidth = 1ULL << 6;
  uint64_t rangeMax = range().second;
  ASSERT_EQ(rangeMin + initWidth, rangeMax);

  arangodb::containers::FnvHashProvider hasher;

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  
  // We grow once to the left, so that initialRangeMin - rangeMin is 2^24.
  // Then we grow to the right until the width is 2^(18+24) = 2^42.
  // The next grow operation after that needs to shift, since then
  // the size of a bucket becomes 2^24 and with the next grow operation
  // the difference initialRangeMin - rangeMin would no longer be divisble
  // by the bucket size.
  growLeft(rangeMin - 1);
  for (int i = 0; i < 17; ++i) {
    growRight(rangeMax);
    rangeMax = range().second;
  }

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(0, count());
  ASSERT_EQ(0, rootValue());
  ASSERT_EQ(rangeMin - initWidth, range().first);
  rangeMin = range().first;
  rangeMax = range().second;
  ASSERT_EQ(rangeMin + (1ULL << 42), rangeMax);
  bucketWidth = (range().second - range().first) >> 18;
  ASSERT_EQ(1ULL << 24, bucketWidth);

  insert(rangeMin);
  insert(rangeMin + bucketWidth);
  insert(rangeMin + 47 * bucketWidth);

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(3, count());
  ASSERT_EQ(rangeMin, range().first);
  ASSERT_EQ(rangeMax, range().second);
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth), rootValue());

  // Now grow to the right:
  insert(rangeMax);

  // Must not throw:
  checkConsistency();

  ASSERT_EQ(6, maxDepth());
  ASSERT_EQ(4, count());
  ASSERT_EQ(hasher.hash(rangeMin) ^ hasher.hash(rangeMin + bucketWidth) ^
            hasher.hash(rangeMin + 47 * bucketWidth) ^
            hasher.hash(rangeMax), rootValue());
  ASSERT_EQ(rangeMin - bucketWidth, range().first);
  ASSERT_EQ(rangeMax + (rangeMax - rangeMin) - bucketWidth, range().second);

  // Now check the bottommost buckets:
  Node& n = node(index(rangeMin, maxDepth()));
  ASSERT_EQ(1, n.count);
  ASSERT_EQ(hasher.hash(rangeMin), n.hash);
  Node& n2 = node(index(rangeMin + bucketWidth, maxDepth()));
  ASSERT_EQ(1, n2.count);
  ASSERT_EQ(hasher.hash(rangeMin + bucketWidth), n2.hash);
  Node& n3 = node(index(rangeMin + 47 * bucketWidth, maxDepth()));
  ASSERT_EQ(1, n3.count);
  ASSERT_EQ(hasher.hash(rangeMin + 47 * bucketWidth), n3.hash);
  Node& n4 = node(index(rangeMax, maxDepth()));
  ASSERT_EQ(1, n4.count);
  ASSERT_EQ(hasher.hash(rangeMax), n4.hash);
}

TEST(MerkleTreeTest, test_diff_with_shift_1) {
  constexpr uint64_t M = 1234567;     // some large constant
  constexpr uint64_t W = 1ULL << 20;  // width, 4 values in each bucket
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(6, M, M + W, M + 16);
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t2(6, M + 16, M + W + 16, M + 16);   // four buckets further right

  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));

  // Now insert something into t1 left of tree 2 as well as in the overlap:
  t1.insert(M);      // first bucket in t1
  expected.emplace_back(std::pair(M, M+3));
  t1.insert(M + 8);  // third bucket in t1
  expected.emplace_back(std::pair(M+8, M+11));
  t1.insert(M + 16); // firth bucket in t1, first bucket in t2
  expected.emplace_back(std::pair(M+16, M+19));
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  t1.clear();
  expected.clear();

  // Now insert something into t1 left of tree 2 as well as in the overlap, but expect one contiguous interval:
  t1.insert(M);      // first bucket in t1
  t1.insert(M + 4);  // second bucket in t1
  t1.insert(M + 8);  // third bucket in t1
  t1.insert(M + 12); // fourth bucket in t1
  t1.insert(M + 16); // firth bucket in t1, first bucket in t2
  expected.emplace_back(std::pair(M, M+19));
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  t1.clear();
  expected.clear();

  // Now insert something into t2 to the right of tree 1 as well as in the overlap:
  t2.insert(M + W - 8);      // second last bucket in t1, 6th last in t2
  expected.emplace_back(std::pair(M + W - 8, M + W - 5));
  t2.insert(M + W);          // not in t1, fourth last bucket in t2
  expected.emplace_back(std::pair(M + W, M + W + 3));
  t2.insert(M + W + 8);      // not in t1, second last bucket in t2
  expected.emplace_back(std::pair(M + W + 8, M + W + 11));
  t2.clear();
  expected.clear();
  
  // Now insert something into t2 to the right of tree 1 as well as in the overlap, but expect one contiguous interval:
  t2.insert(M + W - 8);      // second last bucket in t1, 6th last in t2
  t2.insert(M + W - 4);      // last bucket in t1, 5th last in t2
  t2.insert(M + W);          // not in t1, fourth last bucket in t2
  t2.insert(M + W + 4);      // not in t1, third last bucket in t2
  t2.insert(M + W + 8);      // not in t1, second last bucket in t2
  expected.emplace_back(std::pair(M + W - 8, M + W + 11));
  t2.clear();
  expected.clear();

  // And finally some changes in t1 and some in t2:
  t1.insert(M);
  expected.emplace_back(std::pair(M, M + 3));
  t1.insert(M + 16);
  t2.insert(M + 16);
  // Nothing in this bucket, since both have the same!
  t1.insert(M + 21);
  t2.insert(M + 22);
  expected.emplace_back(std::pair(M + 20, M + 23));
  t1.insert(M + W - 8);
  t2.insert(M + W - 5);
  expected.emplace_back(std::pair(M + W - 8, M + W - 5));
  t2.insert(M + W);
  t2.insert(M + W + 5);
  expected.emplace_back(std::pair(M + W, M + W + 7));
  t2.clear();
}

TEST(MerkleTreeTest, test_diff_empty_random_data_shifted) {
  constexpr uint64_t M = (1ULL << 32) + 17;     // some large constant
  constexpr uint64_t W = 1ULL << 20;  // initial width, 4 values in each bucket
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(6, M, M + W, M + 16);
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t2(6, M + 16, M + W + 16, M + 16);   // four buckets further right

  // Now produce a large list of random keys, in the covered range and beyond on both sides.
  // We then shuffle the vector and insert into both trees in a different order.
  // This will eventually grow the trees in various ways, but in the end, it should always
  // find no differences whatsoever.

  auto rd {std::random_device{}()};
  auto mt {std::mt19937_64(rd)};
  std::uniform_int_distribution<uint64_t> uni{M - (1ULL << 12), M + (1ULL << 28)};
  std::vector<uint64_t> original;
  for (size_t i = 0; i < 100000; ++i) {
    original.push_back(uni(mt));
  }
  std::vector<uint64_t> shuffled{original};
  std::shuffle(shuffled.begin(), shuffled.end(), mt);

  for (auto x : original) {
    t1.insert(x);
  }
  for (auto x : shuffled) {
    t2.insert(x);
  }

  auto t1c = t1.clone();
  auto t2c = t2.clone();

  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, t2, expected));
  ASSERT_TRUE(::diffAsExpected(t2, t1, expected));
}

TEST(MerkleTreeTest, test_clone_compare_clean) {
  constexpr uint64_t M = 1234567;     // some large constant
  constexpr uint64_t W = 1ULL << 20;  // width, 4 values in each bucket
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(6, M, M + W, M + 16);

  // Prepare a tree:
  auto rd {std::random_device{}()};
  auto mt {std::mt19937_64(rd)};
  std::uniform_int_distribution<uint64_t> uni{M, M + (1ULL << 20)};
  std::vector<uint64_t> data;
  for (size_t i = 0; i < 1000; ++i) {
    data.push_back(uni(mt));
  }
  for (auto x : data) {
    t1.insert(x);
  }
  
  // Now clone tree:
  auto t2 = t1.clone();

  // And compare the two:
  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, *t2, expected));

  // And compare bitwise:
  std::string s1;
  t1.serializeBinary(s1, false);
  std::string s2;
  t2->serializeBinary(s2, false);
  ASSERT_EQ(s1, s2);

  s1.clear();
  t1.serializeBinary(s1, true);
  s2.clear();
  t2->serializeBinary(s2, true);
  ASSERT_EQ(s1, s2);

  // Now use operator=
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t3(6, M, M + W, M + 16);

  t3 = std::move(t2);
  // And compare bitwise:
  s1.clear();
  t1.serializeBinary(s1, false);
  s2.clear();
  t3.serializeBinary(s2, false);
  ASSERT_EQ(s1, s2);

  s1.clear();
  t1.serializeBinary(s1, true);
  s2.clear();
  t3.serializeBinary(s2, true);
  ASSERT_EQ(s1, s2);
}

TEST(MerkleTreeTest, test_clone_compare_clean_large) {
  constexpr uint64_t M = 1234567;     // some large constant
  constexpr uint64_t W = 1ULL << 20;  // width, 4 values in each bucket
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(6, M, M + W, M + 16);

  // Prepare a tree:
  auto rd {std::random_device{}()};
  auto mt {std::mt19937_64(rd)};
  std::uniform_int_distribution<uint64_t> uni{M, M + (1ULL << 20)};
  std::vector<uint64_t> data;
  for (size_t i = 0; i < 20000; ++i) {
    data.push_back(uni(mt));
  }
  for (auto x : data) {
    t1.insert(x);
  }
  
  // Now clone tree:
  auto t2 = t1.clone();

  // And compare the two:
  std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
  ASSERT_TRUE(::diffAsExpected(t1, *t2, expected));

  // And compare bitwise:
  std::string s1;
  t1.serializeBinary(s1, false);
  std::string s2;
  t2->serializeBinary(s2, false);
  ASSERT_EQ(s1, s2);

  s1.clear();
  t1.serializeBinary(s1, true);
  s2.clear();
  t2->serializeBinary(s2, true);
  ASSERT_EQ(s1, s2);

  // Now use operator=
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t3(6, M, M + W, M + 16);

  t3 = std::move(t2);
  // And compare bitwise:
  s1.clear();
  t1.serializeBinary(s1, false);
  s2.clear();
  t3.serializeBinary(s2, false);
  ASSERT_EQ(s1, s2);

  s1.clear();
  t1.serializeBinary(s1, true);
  s2.clear();
  t3.serializeBinary(s2, true);
  ASSERT_EQ(s1, s2);
}

TEST(MerkleTreeTest, test_to_string) {
  constexpr uint64_t M = 1234567;     // some large constant
  constexpr uint64_t W = 1ULL << 20;
  ::arangodb::containers::MerkleTree<::arangodb::containers::FnvHashProvider, 3> t1(2, M, M + W, M);

  // Prepare a tree:
  auto rd {std::random_device{}()};
  auto mt {std::mt19937_64(rd)};
  std::uniform_int_distribution<uint64_t> uni{M, M + (1ULL << 20)};
  std::vector<uint64_t> data;
  for (size_t i = 0; i < 100; ++i) {
    data.push_back(uni(mt));
  }
  for (auto x : data) {
    t1.insert(x);
  }
  
  std::string s = t1.toString(false);
  ASSERT_LE(1500, s.size());
  s = t1.toString(true);
  ASSERT_LE(1500, s.size());
}
