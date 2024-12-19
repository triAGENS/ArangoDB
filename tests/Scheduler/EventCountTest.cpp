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
////////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <thread>

#include "Scheduler/EventCount.h"

using namespace arangodb;

TEST(EventCountTest, notify_does_nothing_if_there_are_no_waiters) {
  EventCount ec(1);
  ec.notifyOne();
  EXPECT_EQ(0, ec.getNumSignals());

  ec.notifyAll();
  EXPECT_EQ(0, ec.getNumSignals());
}

TEST(
    EventCountTest,
    prepareWait_increment_numWaiters_cancel_decrement_numWaiters_and_if_necessary_numSignals) {
  EventCount ec(2);
  EXPECT_EQ(0, ec.getNumWaiters());
  auto wait1 = ec.prepareWait(0);
  EXPECT_EQ(1, ec.getNumWaiters());

  auto wait2 = ec.prepareWait(1);
  ec.notifyOne();
  EXPECT_EQ(2, ec.getNumWaiters());
  EXPECT_EQ(1, ec.getNumSignals());

  wait1.cancel();
  EXPECT_EQ(1, ec.getNumWaiters());
  EXPECT_EQ(1, ec.getNumSignals());

  wait2.cancel();
  EXPECT_EQ(0, ec.getNumWaiters());
  EXPECT_EQ(0, ec.getNumSignals());
}

TEST(
    EventCountTest,
    commit_returns_immediately_if_EventCount_is_signaled_and_consumes_the_signal) {
  EventCount ec(2);
  {
    auto wait1 = ec.prepareWait(0);
    auto wait2 = ec.prepareWait(1);
    ec.notifyOne();
    wait1.commit();
    EXPECT_EQ(1, ec.getNumWaiters());
    EXPECT_EQ(0, ec.getNumSignals());
    wait2.cancel();
  }

  {
    auto wait = ec.prepareWait(0);
    ec.notifyAll();
    wait.commit();
    EXPECT_EQ(0, ec.getNumWaiters());
    EXPECT_EQ(0, ec.getNumSignals());
  }
}

TEST(EventCountTest, commit_blocks_until_EventCount_is_signaled) {
  EventCount ec(3);
  std::atomic<unsigned> prepared{0};
  std::atomic<unsigned> done{0};
  auto func = [&](auto idx) {
    auto wait = ec.prepareWait(idx);
    prepared.fetch_add(1);
    wait.commit();
    done.fetch_add(1);
  };

  {
    std::jthread t1(func, 0);
    while (prepared.load() != 1) {
      std::this_thread::yield();
    }
    std::jthread t2(func, 1);
    while (prepared.load() != 2) {
      std::this_thread::yield();
    }
    std::jthread t3(func, 2);
    while (prepared.load() != 3) {
      std::this_thread::yield();
    }
    EXPECT_EQ(0, done.load());
    using IndexStack = std::vector<std::size_t>;
    EXPECT_EQ((IndexStack{2, 1, 0}), ec.getWaiterStack());
    ec.notifyOne();

    while (done.load() != 1) {
      std::this_thread::yield();
    }

    EXPECT_EQ((IndexStack{1, 0}), ec.getWaiterStack());
    ec.notifyAll();
    while (done.load() != 3) {
      std::this_thread::yield();
    }

    EXPECT_EQ(IndexStack{}, ec.getWaiterStack());
  }

  EXPECT_EQ(3, done.load());
}

struct BoundedCounter {
  std::atomic<unsigned> val;
  static constexpr unsigned maxValue = 10;

  BoundedCounter() : val() {}
  ~BoundedCounter() { EXPECT_EQ(0, val.load()); }

  auto countUp() -> bool {
    auto v = val.load(std::memory_order_relaxed);
    do {
      EXPECT_GE(v, 0);
      EXPECT_LE(v, maxValue);
      if (v == maxValue) {
        return false;
      }
    } while (!val.compare_exchange_weak(v, v + 1, std::memory_order_relaxed));
    return true;
  }

  auto countDown() -> bool {
    auto v = val.load(std::memory_order_relaxed);
    do {
      EXPECT_GE(v, 0);
      EXPECT_LE(v, maxValue);
      if (v == 0) {
        return false;
      }
    } while (!val.compare_exchange_weak(v, v - 1, std::memory_order_relaxed));
    return true;
  }

  auto isZero() const -> bool {
    return val.load(std::memory_order_relaxed) == 0;
  }
};

TEST(EventCountTest, stress_test) {
  unsigned const numThreads = std::thread::hardware_concurrency();
  constexpr unsigned numEvents = 1 << 16;
  constexpr unsigned numCounters = 10;

  EventCount ec(numThreads);
  std::array<BoundedCounter, numCounters> counters;

  std::vector<std::jthread> producers;
  for (unsigned i = 0; i < numThreads; ++i) {
    producers.emplace_back([&ec, &counters]() {
      std::mt19937 rnd(
          std::hash<std::thread::id>()(std::this_thread::get_id()));
      unsigned cnt = 0;
      while (cnt < numEvents) {
        unsigned idx = rnd() % numCounters;
        if (counters[idx].countUp()) {
          ec.notifyOne();
          ++cnt;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::jthread> consumers;
  for (unsigned i = 0; i < numThreads; ++i) {
    consumers.emplace_back([&ec, &counters, i]() {
      std::mt19937 rnd(
          std::hash<std::thread::id>()(std::this_thread::get_id()));
      unsigned cnt = 0;
      while (cnt < numEvents) {
        unsigned idx = rnd() % numCounters;
        if (counters[idx].countDown()) {
          ++cnt;
        } else {
          auto wait = ec.prepareWait(i);
          bool allZero = std::all_of(counters.begin(), counters.end(),
                                     [](auto const& c) { return c.isZero(); });
          if (allZero) {
            wait.commit();
          } else {
            wait.cancel();
          }
        }
      }
    });
  }
}
