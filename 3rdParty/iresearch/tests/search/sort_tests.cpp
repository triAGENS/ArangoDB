﻿////////////////////////////////////////////////////////////////////////////////
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

#include <algorithm>

#include "tests_shared.hpp"
#include "analysis/token_attributes.hpp"
#include "search/scorers.hpp"
#include "search/sort.hpp"
#include "utils/misc.hpp"

NS_LOCAL

template<size_t Size, size_t Align>
struct aligned_value {
  irs::memory::aligned_storage<Size, Align> data;

  // need these operators only to be sort API compliant
  bool operator<(const aligned_value&) const noexcept { return false; }
  const aligned_value& operator+=(const aligned_value&) const noexcept { return *this; }
  const aligned_value& operator+(const aligned_value&) const noexcept { return *this; }
};

template<typename ScoreType, typename StatsType>
struct aligned_scorer : public irs::sort {
  class prepared final : public irs::prepared_sort_basic<ScoreType, StatsType> {
   public:
    explicit prepared(const irs::flags& features) noexcept
      : features_(features) {
    }

    virtual field_collector::ptr prepare_field_collector() const override {
      return nullptr;
    }
    virtual term_collector::ptr prepare_term_collector() const override {
      return nullptr;
    }
    virtual void collect(
        irs::byte_type*,
        const irs::index_reader&,
        const field_collector*,
        const term_collector*
    ) const override {
      // NOOP
    }
    virtual std::pair<irs::score_ctx_ptr, irs::score_f> prepare_scorer(
        const irs::sub_reader& segment,
        const irs::term_reader& field,
        const irs::byte_type* stats,
        const irs::attribute_provider& doc_attrs,
        irs::boost_t boost) const override {
      return { nullptr, nullptr };
    }
    virtual const irs::flags& features() const override {
      return features_;
    }

    irs::flags features_;
  };

  static constexpr irs::string_ref type_name() noexcept {
    return __FILE__ ":" STRINGIFY(__LINE__);
  }

  static ptr make(const irs::flags& features = irs::flags::empty_instance()) {
    return std::make_shared<aligned_scorer>(features);
  }
  aligned_scorer(const irs::flags& features = irs::flags::empty_instance())
    : irs::sort(irs::type<aligned_scorer>::get()),
      features_(features) {
  }
  virtual irs::sort::prepared::ptr prepare() const override {
    return irs::memory::make_unique<aligned_scorer<ScoreType, StatsType>::prepared>(features_);
  }

  irs::flags features_;
};

struct dummy_scorer0: public irs::sort {
  static constexpr irs::string_ref type_name() noexcept {
    return __FILE__ ":" STRINGIFY(__LINE__);
  }

  static ptr make() { return std::make_shared<dummy_scorer0>(); }
  dummy_scorer0(): irs::sort(irs::type<dummy_scorer0>::get()) { }
  virtual prepared::ptr prepare() const override { return nullptr; }
};

NS_END

TEST(sort_tests, order_equal) {
  struct dummy_scorer1: public irs::sort {
    static constexpr irs::string_ref type_name() noexcept {
      return __FILE__ ":" STRINGIFY(__LINE__);
    }

    static ptr make() { return std::make_shared<dummy_scorer1>(); }
    dummy_scorer1(): irs::sort(irs::type<dummy_scorer1>::get()) { }
    virtual prepared::ptr prepare() const override { return nullptr; }
  };

  // empty == empty
  {
    irs::order ord0;
    irs::order ord1;
    ASSERT_TRUE(ord0 == ord1);
    ASSERT_FALSE(ord0 != ord1);
  }

  // empty == !empty
  {
    irs::order ord0;
    irs::order ord1;
    ord1.add<dummy_scorer1>(false);
    ASSERT_FALSE(ord0 == ord1);
    ASSERT_TRUE(ord0 != ord1);
  }

  // different sort types
  {
    irs::order ord0;
    irs::order ord1;
    ord0.add<dummy_scorer0>(false);
    ord1.add<dummy_scorer1>(false);
    ASSERT_FALSE(ord0 == ord1);
    ASSERT_TRUE(ord0 != ord1);
  }

  // different order same sort type
  {
    irs::order ord0;
    irs::order ord1;
    ord0.add<dummy_scorer0>(false);
    ord0.add<dummy_scorer1>(false);
    ord1.add<dummy_scorer1>(false);
    ord1.add<dummy_scorer0>(false);
    ASSERT_FALSE(ord0 == ord1);
    ASSERT_TRUE(ord0 != ord1);
  }

  // different number same sorts
  {
    irs::order ord0;
    irs::order ord1;
    ord0.add<dummy_scorer0>(false);
    ord1.add<dummy_scorer0>(false);
    ord1.add<dummy_scorer0>(false);
    ASSERT_FALSE(ord0 == ord1);
    ASSERT_TRUE(ord0 != ord1);
  }

  // different number different sorts
  {
    irs::order ord0;
    irs::order ord1;
    ord0.add<dummy_scorer0>(false);
    ord1.add<dummy_scorer1>(false);
    ord1.add<dummy_scorer1>(false);
    ASSERT_FALSE(ord0 == ord1);
    ASSERT_TRUE(ord0 != ord1);
  }

  // same sorts same types
  {
    irs::order ord0;
    irs::order ord1;
    ord0.add<dummy_scorer0>(false);
    ord0.add<dummy_scorer1>(false);
    ord1.add<dummy_scorer0>(false);
    ord1.add<dummy_scorer1>(false);
    ASSERT_TRUE(ord0 == ord1);
    ASSERT_FALSE(ord0 != ord1);
  }
}

TEST(sort_tests, static_const) {
  ASSERT_TRUE(irs::order::unordered().empty());
  ASSERT_TRUE(irs::order::prepared::unordered().empty());
}

TEST(sort_tests, score_traits) {
  const size_t values[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
  const size_t* ptrs[IRESEARCH_COUNTOF(values)];
  std::iota(std::begin(ptrs), std::end(ptrs), values);

  irs::order_bucket bucket(aligned_scorer<size_t, size_t>().prepare(), 0, 0, true);

  for (size_t i = 0; i < IRESEARCH_COUNTOF(values); ++i) {
    size_t max_dst = 0;
    size_t aggregated_dst = 0;

    irs::score_traits<size_t>::aggregate(
      &bucket,
      reinterpret_cast<irs::byte_type*>(&aggregated_dst),
      reinterpret_cast<const irs::byte_type**>(ptrs), i);

    irs::score_traits<size_t>::max(
      &bucket,
      reinterpret_cast<irs::byte_type*>(&max_dst),
      reinterpret_cast<const irs::byte_type**>(ptrs), i);

    const auto begin = std::begin(values);
    const auto end = begin + i;

    ASSERT_EQ(std::accumulate(begin, end, 0), aggregated_dst);
    const auto it = std::max_element(begin, end);
    ASSERT_EQ(end == it ? 0 : *it, max_dst);
  }
}

TEST(sort_tests, merge_func) {
  aligned_scorer<size_t, size_t> scorer;
  auto prepared = scorer.prepare();
  ASSERT_NE(nullptr, prepared);
  ASSERT_EQ(prepared->aggregate_func(), irs::sort::prepared::merge_func<irs::sort::MergeType::AGGREGATE>(*prepared));
  ASSERT_EQ(&irs::score_traits<size_t>::aggregate, prepared->aggregate_func());
  ASSERT_EQ(prepared->max_func(), irs::sort::prepared::merge_func<irs::sort::MergeType::MAX>(*prepared));
  ASSERT_EQ(&irs::score_traits<size_t>::max, prepared->max_func());

  // ensure order optimizes single scorer cases
  {
    irs::order ord;
    ord.add<aligned_scorer<size_t, size_t>>(true);

    auto prepared_order = ord.prepare();
    ASSERT_FALSE(prepared_order.empty());

    ASSERT_EQ(prepared_order.prepare_merger(irs::sort::MergeType::AGGREGATE), prepared->aggregate_func());
    ASSERT_EQ(prepared_order.prepare_merger(irs::sort::MergeType::MAX), prepared->max_func());
  }
}

TEST(sort_tests, prepare_order) {
  {
    irs::order ord;
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<1, 4>, aligned_value<1, 4>>>(true);

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 }, // score: 0-0
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags::empty_instance(), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(1, prepared.size());
    ASSERT_EQ(4, prepared.score_size());
    ASSERT_EQ(4, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_TRUE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(true);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(true);
    ord.add<aligned_scorer<aligned_value<4, 4>, aligned_value<4, 4>>>(true);

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 }, // score: 0-1
      { 2, 2 }, // score: 2-3
      { 4, 4 }, // score: 4-7
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags::empty_instance(), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(3, prepared.size());
    ASSERT_EQ(8, prepared.score_size());
    ASSERT_EQ(8, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_TRUE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(true);
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(true);
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(true);

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 }, // score: 0-0
      { 1, 1 }, // score: 1-1
      { 2, 2 }  // score: 2-2
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags::empty_instance(), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(3, prepared.size());
    ASSERT_EQ(3, prepared.score_size());
    ASSERT_EQ(3, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_TRUE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(true);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(true);
    ord.add<dummy_scorer0>(false);

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 }, // score: 0-0, padding: 1-1
      { 2, 2 }  // score: 2-3
    };

    auto prepared = ord.prepare();
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(irs::flags::empty_instance(), prepared.features());
    ASSERT_EQ(2, prepared.size());
    ASSERT_EQ(4, prepared.score_size());
    ASSERT_EQ(4, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_TRUE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(true);
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(true);
    ord.add<aligned_scorer<aligned_value<4, 4>, aligned_value<4, 4>>>(true);

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags::empty_instance(), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(3, prepared.size());
    ASSERT_EQ(8, prepared.score_size());
    ASSERT_EQ(8, prepared.stats_size());

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 }, // score: 0-0, padding: 1-1
      { 2, 2 }, // score: 2-3
      { 4, 4 }  // score: 4-7
    };

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_TRUE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<5, 4>, aligned_value<5, 4>>>(false);
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 },  // score: 0-0, padding: 1-3
      { 4, 4 },  // score: 4-8, padding: 9-11
      { 12, 12 } // score: 12-14
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(3, prepared.size());
    ASSERT_EQ(16, prepared.score_size());
    ASSERT_EQ(16, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<3, 1>, aligned_value<3, 1>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<7, 4>, aligned_value<7, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<dummy_scorer0>(false);
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<dummy_scorer0>(false);

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 },   // score: 0-2, padding: 3-7
      { 8, 8 },   // score: 8-34, padding: 35-39
      { 40, 40 }, // score: 40-46, padding: 47-47
      { 48, 48 }, // score: 48-48
      { 49, 49 }  // score: 49-49
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(56, prepared.score_size());
    ASSERT_EQ(56, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<aligned_scorer<aligned_value<3, 1>, aligned_value<3, 1>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<7, 4>, aligned_value<7, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 },   // score: 0-26, padding: 27-31
      { 32, 32 }, // score: 32-34, padding: 34-35
      { 36, 36 }, // score: 36-42, padding: 43-43
      { 44, 44 }, // score: 44-44
      { 45, 45 }  // score: 45-45
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(48, prepared.score_size());
    ASSERT_EQ(48, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<aligned_scorer<aligned_value<7, 4>, aligned_value<7, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<3, 1>, aligned_value<3, 1>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0, 0 },   // score: 0-26, padding: 27-31
      { 32, 32 }, // score: 32-38, padding: 39-39
      { 40, 40 }, // score: 40-42
      { 43, 43 }, // score: 43-43
      { 44, 44 }  // score: 44-44
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(48, prepared.score_size());
    ASSERT_EQ(48, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<4, 4>, aligned_value<4, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0,  0 },  // score: 0-26, padding: 27-31
      { 32, 32 }, // score: 32-33, padding: 34-35
      { 36, 36 }, // score: 36-39
      { 40, 40 }, // score: 40-40
      { 41, 41 }  // score: 41-41
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(48, prepared.score_size());
    ASSERT_EQ(48, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<aligned_scorer<aligned_value<4, 4>, aligned_value<4, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0,  0  }, // score: 0-26, padding: 27-31
      { 32, 32 }, // score: 32-35
      { 36, 36 }, // score: 36-37
      { 38, 38 }, // score: 38-38
      { 39, 39 }  // score: 39-39
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(40, prepared.score_size());
    ASSERT_EQ(40, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }

  {
    irs::order ord;
    ord.add<aligned_scorer<aligned_value<27, 8>, aligned_value<27, 8>>>(false);
    ord.add<aligned_scorer<aligned_value<4, 4>, aligned_value<4, 4>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<2, 2>, aligned_value<2, 2>>>(false, irs::flags{irs::type<irs::document>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});
    ord.add<aligned_scorer<aligned_value<1, 1>, aligned_value<1, 1>>>(false, irs::flags{irs::type<irs::frequency>::get()});

    // first - score offset
    // second - stats offset
    const std::vector<std::pair<size_t, size_t>> expected_offsets {
      { 0,  0  }, // score: 0-26, padding: 27-31
      { 32, 32 }, // score: 32-35
      { 36, 36 }, // score: 36-37
      { 38, 38 }, // score: 38-38
      { 39, 39 }  // score: 39-39
    };

    auto prepared = ord.prepare();
    ASSERT_EQ(irs::flags({ irs::type<irs::document>::get(), irs::type<irs::frequency>::get() }), prepared.features());
    ASSERT_FALSE(prepared.empty());
    ASSERT_EQ(5, prepared.size());
    ASSERT_EQ(40, prepared.score_size());
    ASSERT_EQ(40, prepared.stats_size());

    auto expected_offset = expected_offsets.begin();
    for (auto& bucket : prepared) {
      ASSERT_NE(nullptr, bucket.bucket);
      ASSERT_EQ(expected_offset->first, bucket.score_offset);
      ASSERT_EQ(expected_offset->second, bucket.stats_offset);
      ASSERT_FALSE(bucket.reverse);
      ++expected_offset;
    }
    ASSERT_EQ(expected_offset, expected_offsets.end());
  }
}
