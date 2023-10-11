#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "Logger/LogMacros.h"

#include "Aql/IndexJoin/TwoIndicesUniqueMergeJoin.h"

using namespace arangodb;

namespace {
using MyKeyValue = std::size_t;
using MyDocumentId = std::size_t;

using Strategy =
    arangodb::aql::TwoIndicesUniqueMergeJoin<MyKeyValue, MyDocumentId>;
using MyIndexStreamIterator = Strategy::StreamIteratorType;

using Desc = Strategy::Descriptor;

struct MyVectorIterator : MyIndexStreamIterator {
  bool position(std::span<MyKeyValue> sp) const override {
    if (current != end) {
      sp[0] = *current;
      return true;
    }
    return false;
  }

  bool seek(std::span<MyKeyValue> key) override {
    current = std::lower_bound(
        begin, end, key[0], [](auto left, auto right) { return left < right; });
    if (current != end) {
      key[0] = *current;
      return true;
    }
    return false;
  }

  MyDocumentId load(std::span<MyKeyValue> projections) const override {
    return *current;
  }

  void cacheCurrentKey(std::span<MyKeyValue> cache) override {
    cache[0] = *current;
  }

  bool reset(std::span<MyKeyValue> span) override {
    current = begin;
    if (current != end) {
      span[0] = *current;
    }
    return current != end;
  }

  bool next(std::span<MyKeyValue> key, MyDocumentId& doc,
            std::span<MyKeyValue> projections) override {
    current = current + 1;
    if (current == end) {
      return false;
    }

    key[0] = *current;
    doc = *current;
    return true;
  }

  using Container = std::span<MyKeyValue>;

  Container::iterator begin;
  Container::iterator current;
  Container::iterator end;

  MyVectorIterator(Container c)
      : begin(c.begin()), current(begin), end(c.end()) {}
};

}  // namespace

TEST(UniqueIndexMerger, no_results) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {
      1,
      3,
      5,
      7,
  };

  std::vector<MyKeyValue> b = {
      2, 4, 6, 8, 10,
  };

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 0);
}

TEST(UniqueIndexMerger, some_results) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {
      1, 3, 5, 7, 8, 9,
  };

  std::vector<MyKeyValue> b = {
      2, 4, 6, 8, 10,
  };

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 1);
}

TEST(UniqueIndexMerger, one_empty) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {};

  std::vector<MyKeyValue> b = {
      2, 4, 6, 8, 10,
  };

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 0);
}

TEST(UniqueIndexMerger, both_empty) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {};

  std::vector<MyKeyValue> b = {};

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 0);
}

TEST(UniqueIndexMerger, product_result) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {1, 1};
  std::vector<MyKeyValue> b = {1, 1};

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 4);
}

TEST(UniqueIndexMerger, two_phase_product_result) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {1, 1, 3, 4};

  std::vector<MyKeyValue> b = {1, 1, 2, 4};

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 5);
}

TEST(UniqueIndexMerger, two_phase_product_result_two_streaks) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {1, 1, 2, 2};

  std::vector<MyKeyValue> b = {1, 1, 2, 2};

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);
  iters.emplace_back(std::make_unique<MyVectorIterator>(b), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs[0], docs[1]);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, 4 + 4);
}

TEST(UniqueIndexMerger, one_iterator_corner_case) {
  bool isUnique = true;
  std::vector<MyKeyValue> a = {0, 1, 2, 3};

  std::vector<Desc> iters;
  iters.emplace_back(std::make_unique<MyVectorIterator>(a), 0, isUnique);

  Strategy merger{std::move(iters), 1};

  bool hasMore = true;
  std::size_t count = 0;
  while (hasMore) {
    hasMore =
        merger.next([&](std::span<MyDocumentId> docs, std::span<MyKeyValue>) {
          EXPECT_EQ(docs.size(), 1);
          LOG_DEVEL << docs[0];
          EXPECT_EQ(docs[0], count);
          count += 1;
          return true;
        });
  }

  ASSERT_EQ(count, a.size());
}
