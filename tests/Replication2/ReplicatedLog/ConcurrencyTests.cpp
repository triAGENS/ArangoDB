////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include <velocypack/Slice.h>

#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/Helper/TestHelper.h"

#include <Basics/ThreadGuard.h>
#include <Basics/ScopeGuard.h>
#include <Basics/application-exit.h>

#include <functional>

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::test;

struct ReplicatedLogConcurrentTest : ReplicatedLogTest {
  using ThreadIdx = uint16_t;
  using IterIdx = uint32_t;
  constexpr static auto maxIter = std::numeric_limits<IterIdx>::max();

  struct alignas(128) ThreadCoordinationData {
    // the testee
    std::shared_ptr<ILogParticipant> log;

    // only when set to true, all client threads start
    std::atomic<bool> go = false;
    // when set to true, client threads will stop after the current iteration,
    // whatever that means for them.
    std::atomic<bool> stopClientThreads = false;
    // when set to true, the replication thread stops. should be done only
    // after all client threads stopped to avoid them hanging while waiting on
    // replication.
    std::atomic<bool> stopReplicationThreads = false;
    // every thread increases this by one when it's ready to start
    std::atomic<std::size_t> threadsReady = 0;
    // every thread increases this by one when it's done a certain minimal
    // amount of work. This is to guarantee that all threads are running long
    // enough side by side.
    std::atomic<std::size_t> threadsSatisfied = 0;
  };

  // Used to generate payloads that are unique across threads.
  constexpr static auto genPayload = [](ThreadIdx thread, IterIdx i) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
    // up to 5 digits for thread
    static_assert(std::numeric_limits<ThreadIdx>::max() <= 99'999);
    // up to 10 digits for i
    static_assert(std::numeric_limits<IterIdx>::max() <= 99'999'999'999);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    // This should fit into short string optimization
    auto str = std::string(16, ' ');

    auto p = str.begin() + 5;
    // 1 digit for ':'
    *p = ':';

    do {
      --p;
      *p = static_cast<char>('0' + (thread % 10));
      thread /= 10;
    } while (thread > 0);
    p = str.end();
    do {
      --p;
      *p = static_cast<char>('0' + (i % 10));
      i /= 10;
    } while (i > 0);

    return str;
  };

  constexpr static auto alternatinglyInsertAndRead =
      [](ThreadIdx const threadIdx, ThreadCoordinationData& data) {
        using namespace std::chrono_literals;
        auto log = std::dynamic_pointer_cast<LogLeader>(data.log);
        ASSERT_NE(log, nullptr);
        data.threadsReady.fetch_add(1);
        while (!data.go.load()) {
        }

        for (auto i = std::uint32_t{0};
             i < maxIter && !data.stopClientThreads.load(); ++i) {
          auto const payload =
              LogPayload::createFromString(genPayload(threadIdx, i));
          auto const idx = log->insert(payload, false,
                                       LogLeader::doNotTriggerAsyncReplication);
          std::this_thread::sleep_for(1ns);
          auto fut = log->waitFor(idx);
          fut.get();
          auto snapshot = log->getReplicatedLogSnapshot();
          ASSERT_LT(0U, idx.value);
          ASSERT_LE(idx.value, snapshot.size());
          auto const& entry = snapshot[idx.value - 1];
          EXPECT_EQ(idx, entry.entry().logIndex());
          ASSERT_TRUE(entry.entry().hasPayload());
          EXPECT_EQ(payload, *entry.entry().logPayload());
          if (i == 1000) {
            // we should have done at least a few iterations before finishing
            data.threadsSatisfied.fetch_add(1, std::memory_order_relaxed);
          }
        }
      };

  constexpr static auto insertManyThenRead = [](ThreadIdx const threadIdx,
                                                ThreadCoordinationData& data) {
    using namespace std::chrono_literals;
    auto log = std::dynamic_pointer_cast<LogLeader>(data.log);
    ASSERT_NE(log, nullptr);
    data.threadsReady.fetch_add(1);
    while (!data.go.load()) {
    }

    constexpr auto batch = 100;
    auto idxs = std::vector<LogIndex>{};
    idxs.resize(batch);

    for (auto i = std::uint32_t{0};
         i < maxIter && !data.stopClientThreads.load(); i += batch) {
      for (auto k = 0; k < batch && i + k < maxIter; ++k) {
        auto const payload =
            LogPayload::createFromString(genPayload(threadIdx, i + k));
        idxs[k] = log->insert(payload, false,
                              LogLeader::doNotTriggerAsyncReplication);
      }
      std::this_thread::sleep_for(1ns);
      auto fut = log->waitFor(idxs.back());
      fut.get();
      auto snapshot = log->getReplicatedLogSnapshot();
      for (auto k = 0; k < batch && i + k < maxIter; ++k) {
        using namespace std::string_literals;
        auto const payload = std::optional(
            LogPayload::createFromString(genPayload(threadIdx, i + k)));
        auto const idx = idxs[k];
        ASSERT_LT(0U, idx.value);
        ASSERT_LE(idx.value, snapshot.size());
        auto const& entry = snapshot[idx.value - 1];
        EXPECT_EQ(idx, entry.entry().logIndex());
        ASSERT_TRUE(entry.entry().hasPayload());
        EXPECT_EQ(payload, *entry.entry().logPayload())
            << payload->slice().toJson() << " "
            << (entry.entry().logPayload() ? payload->slice().toJson()
                                           : "std::nullopt"s);
      }
      if (i == 10 * batch) {
        // we should have done at least a few iterations before finishing
        data.threadsSatisfied.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  constexpr static auto runReplicationWithIntermittentPauses =
      [](ThreadCoordinationData& data) {
        using namespace std::chrono_literals;
        auto log = std::dynamic_pointer_cast<LogLeader>(data.log);
        ASSERT_NE(log, nullptr);
        for (auto i = 0;; ++i) {
          log->triggerAsyncReplication();
          if (i % 16) {
            std::this_thread::sleep_for(100ns);
            if (data.stopReplicationThreads.load()) {
              return;
            }
          }
        }
      };

  constexpr static auto runFollowerReplicationWithIntermittentPauses =
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      [](std::vector<DelayedFollowerLog*> followers,
         ThreadCoordinationData& data) {
        using namespace std::chrono_literals;
        for (auto i = 0;;) {
          for (auto* follower : followers) {
            follower->runAsyncAppendEntries();
            if (i % 17) {
              std::this_thread::sleep_for(100ns);
              if (data.stopReplicationThreads.load()) {
                return;
              }
            }
            ++i;
          }
        }
      };
};

TEST_F(ReplicatedLogConcurrentTest, genPayloadTest) {
  EXPECT_EQ("    0:         0", genPayload(0, 0));
  EXPECT_EQ("   11:        42", genPayload(11, 42));
  EXPECT_EQ("65535:4294967295", genPayload(65535, 4294967295));
}

TEST_F(ReplicatedLogConcurrentTest, lonelyLeader) {
  using namespace std::chrono_literals;

  auto replicatedLog = makeReplicatedLogWithAsyncMockLog(LogId{1});
  auto leaderLog = replicatedLog->becomeLeader("leader", LogTerm{1}, {}, 1);

  auto data = ThreadCoordinationData{leaderLog};

  // start replication
  auto replicationThread =
      std::thread{runReplicationWithIntermittentPauses, std::ref(data)};

  auto clientThreads = ThreadGuard(2);

  auto threadCounter = uint16_t{0};
  clientThreads.emplace(alternatinglyInsertAndRead, threadCounter++,
                        std::ref(data));
  clientThreads.emplace(insertManyThenRead, threadCounter++, std::ref(data));

  ASSERT_EQ(clientThreads.size(), threadCounter);
  while (data.threadsReady.load() < clientThreads.size()) {
  }
  data.go.store(true);
  while (data.threadsSatisfied.load() < clientThreads.size()) {
    std::this_thread::sleep_for(100us);
  }
  data.stopClientThreads.store(true);

  clientThreads.joinAll();

  // stop replication only after all client threads joined, so we don't block
  // them in some intermediate state
  data.stopReplicationThreads.store(true);
  replicationThread.join();

  auto stats = std::get<LeaderStatus>(data.log->getStatus().getVariant()).local;
  EXPECT_LE(LogIndex{8000}, stats.commitIndex);
  EXPECT_LE(stats.commitIndex, stats.spearHead.index);
  stopAsyncMockLogs();
}

TEST_F(ReplicatedLogConcurrentTest, leaderWithFollowers) {
  using namespace std::chrono_literals;

  auto guard = scopeGuard([]() noexcept {
    LOG_TOPIC("27bc7", FATAL, Logger::REPLICATION2)
        << "Test terminating early, aborting for debugging";
    FATAL_ERROR_ABORT();
  });

  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower1Log = makeReplicatedLog(LogId{2});
  auto follower2Log = makeReplicatedLog(LogId{3});

  auto follower1 =
      follower1Log->becomeFollower("follower1", LogTerm{1}, "leader");
  auto follower2 =
      follower2Log->becomeFollower("follower2", LogTerm{1}, "leader");
  auto leader = leaderLog->becomeLeader(
      "leader", LogTerm{1},
      {std::static_pointer_cast<AbstractFollower>(follower1),
       std::static_pointer_cast<AbstractFollower>(follower2)},
      2);

  auto data = ThreadCoordinationData{leader};

  // start replication
  auto replicationThread =
      std::thread{runReplicationWithIntermittentPauses, std::ref(data)};
  auto followerReplicationThread = std::thread{
      runFollowerReplicationWithIntermittentPauses,
      std::vector{follower1.get(), follower2.get()}, std::ref(data)};

  auto clientThreads = ThreadGuard(2);

  auto threadCounter = uint16_t{0};
  clientThreads.emplace(alternatinglyInsertAndRead, threadCounter++,
                        std::ref(data));
  clientThreads.emplace(insertManyThenRead, threadCounter++, std::ref(data));

  ASSERT_EQ(clientThreads.size(), threadCounter);
  while (data.threadsReady.load() < clientThreads.size()) {
  }
  data.go.store(true);
  while (data.threadsSatisfied.load() < clientThreads.size()) {
    std::this_thread::sleep_for(100us);
  }
  data.stopClientThreads.store(true);

  clientThreads.joinAll();

  // stop replication only after all client threads joined, so we don't block
  // them in some intermediate state
  data.stopReplicationThreads.store(true);
  replicationThread.join();
  followerReplicationThread.join();

  guard.cancel();

  auto stats = std::get<LeaderStatus>(data.log->getStatus().getVariant()).local;
  EXPECT_LE(LogIndex{8000}, stats.commitIndex);
  EXPECT_LE(stats.commitIndex, stats.spearHead.index);
}
