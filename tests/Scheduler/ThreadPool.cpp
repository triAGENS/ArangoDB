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
#include <atomic>
#include <chrono>
#include <concepts>
#include <memory>
#include <ratio>
#include <thread>
#include "GeneralServer/RequestLane.h"
#include "Scheduler/SimpleThreadPool.h"
#include "Scheduler/SupervisedScheduler.h"
#include "Mocks/Servers.h"
#include "Metrics/MetricsFeature.h"

using namespace arangodb;

TEST(ThreadPoolTest, start_stop_test) {
  SimpleThreadPool pool{"test-sched", 1};
}

TEST(ThreadPoolTest, simple_counter) {
  std::atomic<std::size_t> counter{0};
  {
    SimpleThreadPool pool{"test-sched", 1};
    pool.push([&]() noexcept { counter++; });
    pool.push([&]() noexcept { counter++; });
    pool.push([&]() noexcept { counter++; });
  }

  ASSERT_EQ(counter, 3);
}

TEST(ThreadPoolTest, multi_thread_counter) {
  std::atomic<std::size_t> counter{0};
  {
    SimpleThreadPool pool{"test-sched", 3};
    for (size_t k = 0; k < 100; k++) {
      pool.push([&]() noexcept { counter++; });
    }
  }

  ASSERT_EQ(counter, 100);
}

TEST(ThreadPoolTest, stop_when_sleeping) {
  {
    SimpleThreadPool pool{"test-sched", 3};
    std::this_thread::sleep_for(std::chrono::seconds{3});
  }
}

TEST(ThreadPoolTest, work_when_sleeping) {
  std::atomic<std::size_t> counter{0};
  {
    SimpleThreadPool pool{"test-sched", 3};
    std::this_thread::sleep_for(std::chrono::seconds{3});
    pool.push([&]() noexcept { counter++; });
    pool.push([&]() noexcept { counter++; });
    pool.push([&]() noexcept { counter++; });
  }
  ASSERT_EQ(counter, 3);
}

struct SupervisedSchedulerPool {
  static constexpr auto limit = 1024 * 64;

  explicit SupervisedSchedulerPool(
      tests::mocks::MockRestServer& mockApplicationServer, unsigned numThreads)
      : metricsFeature(std::make_shared<arangodb::metrics::MetricsFeature>(
            mockApplicationServer.server())),
        scheduler(mockApplicationServer.server(), numThreads, numThreads, limit,
                  limit, limit, limit, limit, 0.0,
                  std::make_shared<SchedulerMetrics>(*metricsFeature)) {
    scheduler.start();
  }

  ~SupervisedSchedulerPool() { scheduler.shutdown(); }

  // we create multiple schedulers, so each one needs its own metrics feature to
  // register its metrics
  std::shared_ptr<arangodb::metrics::MetricsFeature> metricsFeature;
  SupervisedScheduler scheduler;

  template<std::invocable Fn>
  void push(Fn&& fn) {
    scheduler.queue(RequestLane::CLIENT_FAST, std::forward<Fn>(fn));
  }
};

template<class T>
struct PoolBuilder;

template<>
struct PoolBuilder<SimpleThreadPool> {
  SimpleThreadPool makePool(const char* name, unsigned numThreads) {
    return SimpleThreadPool{name, numThreads};
  }
};

template<>
struct PoolBuilder<SupervisedSchedulerPool> {
  tests::mocks::MockRestServer mockApplicationServer;

  SupervisedSchedulerPool makePool(const char*, unsigned numThreads) {
    return SupervisedSchedulerPool{mockApplicationServer, numThreads};
  }
};

template<typename Pool>
class ThreadPoolPerfTest : public testing::Test {};

using PoolTypes = ::testing::Types<SimpleThreadPool, SupervisedSchedulerPool>;
TYPED_TEST_SUITE(ThreadPoolPerfTest, PoolTypes);

namespace {
template<typename Pool>
struct callable;

template<typename Pool>
callable<Pool> createLambda(std::atomic<std::uint64_t>& cnt, Pool& pool, int x,
                            std::atomic<bool>& stop);

template<typename Pool>
struct callable {
  void operator()() const noexcept {
    if (!stop.load()) {
      cnt.fetch_add(1);
      pool.push(createLambda(cnt, pool, x + 1, stop));
      pool.push(createLambda(cnt, pool, x + 1, stop));

      // simulate some work
      unsigned i = 0;
      static constexpr unsigned workLimit = 2 << 13;
      while (!stop.load() && ++i < workLimit) {
      }
    }
  }

  std::atomic<std::uint64_t>& cnt;
  std::atomic<bool>& stop;
  Pool& pool;
  const int x;
};

template<typename Pool>
callable<Pool> createLambda(std::atomic<std::uint64_t>& cnt, Pool& pool, int x,
                            std::atomic<bool>& stop) {
  return callable<Pool>{cnt, stop, pool, x};
}
}  // namespace

template<typename Pool>
void spawnWorkTest(unsigned numThreads) {
  std::atomic<bool> stop = false;
  std::atomic<std::uint64_t> counter = 0;

  std::uint64_t durationMs = 0;
  {
    PoolBuilder<Pool> poolBuilder;
    auto pool = poolBuilder.makePool("pool", numThreads);

    auto start = std::chrono::steady_clock::now();
    pool.push(createLambda(counter, pool, 0, stop));

    std::this_thread::sleep_for(std::chrono::seconds{5});
    stop.store(true);
    durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();

    // wait a bit so we don't run into an assertion in the SupervisedScheduler
    // that we tried to queue an item after the SchedulerFeature was stopped
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  auto numOps = counter.load();
  std::cout << "Throughput: " << numOps / durationMs << " ops/ms\n";
}

TYPED_TEST(ThreadPoolPerfTest, spawn_work_1_thread) {
  if constexpr (std::same_as<TypeParam, SupervisedSchedulerPool>) {
    // the SupervisedScheduler needs at least 4 threads, otherwise it will
    // assert
    std::cout << "Skipping test for SupervisedSchedulerPool\n";
    return;
  }
  spawnWorkTest<TypeParam>(1);
}

TYPED_TEST(ThreadPoolPerfTest, spawn_work_5_threads) {
  spawnWorkTest<TypeParam>(5);
}

TYPED_TEST(ThreadPoolPerfTest, spawn_work_11_threads) {
  spawnWorkTest<TypeParam>(11);
}

TYPED_TEST(ThreadPoolPerfTest, spawn_work_19_threads) {
  spawnWorkTest<TypeParam>(19);
}

template<typename Pool>
struct PingPong {
  PingPong(Pool* pool1, Pool* pool2, int ping, std::atomic<bool>& stop,
           std::atomic<std::uint64_t>& counter)
      : pools{pool1, pool2}, ping(ping), stop(stop), counter(counter) {}

  void operator()() noexcept {
    if (!stop.load()) {
      ping = (ping + 1) & 1;
      pools[ping]->push(PingPong(pools[0], pools[1], ping, stop, counter));
      counter.fetch_add(1);
    }
  }

  Pool* pools[2];
  int ping;
  std::atomic<bool>& stop;
  std::atomic<std::uint64_t>& counter;
};

template<typename Pool>
void pingPongTest(unsigned numThreads) {
  std::atomic<bool> stopSignal = false;
  std::atomic<std::uint64_t> counter = 0;

  std::uint64_t durationMs = 0;
  {
    PoolBuilder<Pool> poolBuilder;
    auto pool1 = poolBuilder.makePool("pool1", 8);
    auto pool2 = poolBuilder.makePool("pool2", 8);

    auto start = std::chrono::steady_clock::now();
    pool1.push(PingPong<Pool>(&pool1, &pool2, 0, stopSignal, counter));

    std::this_thread::sleep_for(std::chrono::seconds{5});
    stopSignal.store(true);

    durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();

    // wait a bit so we don't run into an assertion in the SupervisedScheduler
    // that we tried to queue an item after the SchedulerFeature was stopped
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  auto numOps = counter.load();
  std::cout << "Throughput: " << numOps / durationMs << " ops/ms\n";
}

TYPED_TEST(ThreadPoolPerfTest, ping_pong_1_thread) {
  if constexpr (std::same_as<TypeParam, SupervisedSchedulerPool>) {
    // the SupervisedScheduler needs at least 4 threads, otherwise it will
    // assert
    std::cout << "Skipping test for SupervisedSchedulerPool\n";
    return;
  }
  pingPongTest<TypeParam>(1);
}

TYPED_TEST(ThreadPoolPerfTest, ping_pong_5_threads) {
  pingPongTest<TypeParam>(5);
}

TYPED_TEST(ThreadPoolPerfTest, ping_pong_13_threads) {
  pingPongTest<TypeParam>(13);
}
