////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#include "gtest/gtest.h"

#include "Cluster/RebootTracker.h"
#include "Scheduler/SchedulerFeature.h"
#include "Scheduler/SupervisedScheduler.h"

#include <memory>
#include <type_traits>

using namespace arangodb;
using namespace arangodb::cluster;

class CallbackGuardTest : public ::testing::Test {
 protected:
  uint64_t counterA{0};
  uint64_t counterB{0};
  std::function<void(void)> incrCounterA;
  std::function<void(void)> incrCounterB;

  void SetUp() {
    counterA = 0;
    counterB = 0;
    incrCounterA = [& counterA = this->counterA]() { ++counterA; };
    incrCounterB = [& counterB = this->counterB]() { ++counterB; };
  }
};

TEST_F(CallbackGuardTest, test_default_constructor) {
  // should do nothing, especially not cause an error during destruction
  CallbackGuard guard{};
}

TEST_F(CallbackGuardTest, test_deleted_copy_semantics) {
  ASSERT_FALSE(std::is_copy_constructible<CallbackGuard>::value)
      << "CallbackGuard should not be copy constructible";
  ASSERT_FALSE(std::is_copy_assignable<CallbackGuard>::value)
      << "CallbackGuard should not be copy assignable";
}

TEST_F(CallbackGuardTest, test_constructor) {
  {
    CallbackGuard guard{incrCounterA};
    ASSERT_EQ(0, counterA) << "construction should not invoke the callback";
  }
  ASSERT_EQ(1, counterA) << "destruction should invoke the callback";
}

TEST_F(CallbackGuardTest, test_move_constructor_inline) {
  {
    CallbackGuard guard{CallbackGuard(incrCounterA)};
    ASSERT_EQ(0, counterA)
        << "move construction should not invoke the callback";
  }
  ASSERT_EQ(1, counterA) << "destruction should invoke the callback";
}

TEST_F(CallbackGuardTest, test_move_constructor_explicit) {
  {
    CallbackGuard guardA1{incrCounterA};
    ASSERT_EQ(0, counterA) << "construction should not invoke the callback";
    {
      CallbackGuard guardA2{std::move(guardA1)};
      ASSERT_EQ(0, counterA)
          << "move construction should not invoke the callback";
    }
    ASSERT_EQ(1, counterA)
        << "destroying a move constructed guard should invoke the callback";
  }

  ASSERT_EQ(1, counterA)
      << "destroying a moved guard should not invoke the callback";
}

TEST_F(CallbackGuardTest, test_move_operator_eq_construction) {
  {
    auto guard = CallbackGuard{incrCounterA};
    ASSERT_EQ(0, counterA)
        << "initialization with operator= should not invoke the callback";
  }
  ASSERT_EQ(1, counterA) << "destruction should invoke the callback";
}

TEST_F(CallbackGuardTest, test_move_operator_eq_explicit) {
  {
    CallbackGuard guardA{incrCounterA};
    ASSERT_EQ(0, counterA) << "construction should not invoke the callback";
    {
      CallbackGuard guardB{incrCounterB};
      ASSERT_EQ(0, counterB) << "construction should not invoke the callback";
      guardA = std::move(guardB);
      ASSERT_EQ(0, counterB) << "being moved should not invoke the callback";
      ASSERT_EQ(1, counterA) << "being overwritten should invoke the callback";
    }
    ASSERT_EQ(0, counterB)
        << "destroying a moved guard should not invoke the callback";
    ASSERT_EQ(1, counterA) << "destroying a moved guard should not invoke the "
                              "overwritten callback again";
  }
  ASSERT_EQ(1, counterB)
      << "destroying an overwritten guard should invoke its new callback";
  ASSERT_EQ(1, counterA) << "destroying an overwritten guard should not invoke "
                            "its old callback again";
}

class RebootTrackerTest : public ::testing::Test {
 protected:
  RebootTrackerTest() : scheduler(2, 64, 128, 1024 * 1024, 4096) {}
  using PeerState = RebootTracker::PeerState;

  SupervisedScheduler scheduler;
  static_assert(std::is_same<decltype(SchedulerFeature::SCHEDULER), decltype(&scheduler)>::value,
                "Use the correct scheduler in the tests");

  void SetUp() { scheduler.start(); }
  void TearDown() { scheduler.shutdown(); }

  bool schedulerEmpty() const {
    auto stats = scheduler.queueStatistics();

    return stats._blocked == 0 && stats._queued == 0 && stats._working == 0;
  }

  void waitForSchedulerEmpty() const {
    while (!schedulerEmpty()) {
      std::this_thread::yield();
    }
  }

  static ServerID const serverA;
  static ServerID const serverB;
  static ServerID const serverC;
};

ServerID const RebootTrackerTest::serverA = "PRMR-srv-A";
ServerID const RebootTrackerTest::serverB = "PRMR-srv-B";
ServerID const RebootTrackerTest::serverC = "PRMR-srv-C";

// Test that a registered callback is called once on the next change, but not
// after that
TEST_F(RebootTrackerTest, call_once_after_change) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{1}}};

  uint64_t numCalled = 0;
  auto callback = [&numCalled]() { ++numCalled; };

  {
    RebootTracker rebootTracker{&scheduler};
    std::vector<CallbackGuard> guards{};
    CallbackGuard guard;

    // Set state to { serverA => 1 }
    rebootTracker.updateServerState(state);

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";

    // Set state to { serverA => 2 }
    state.at(serverA) = RebootId{2};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must be called after a change";

    // Set state to { serverA => 3 }
    state.at(serverA) = RebootId{3};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must not be called twice";

    guards.clear();
    ASSERT_EQ(1, numCalled)
        << "Callback must not be called when guards are destroyed";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(1, numCalled) << "Callback must not be called during destruction";
}

// Test that a registered callback is called immediately when its reboot id
// is lower than the last known one, but not after that
TEST_F(RebootTrackerTest, call_once_with_old_rebootid) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{2}}};

  uint64_t numCalled = 0;
  auto callback = [&numCalled]() { ++numCalled; };

  {
    RebootTracker rebootTracker{&scheduler};
    std::vector<CallbackGuard> guards{};
    CallbackGuard guard;

    // Set state to { serverA => 2 }
    rebootTracker.updateServerState(state);

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled)
        << "Callback with lower value must be called immediately";

    // Set state to { serverA => 3 }
    state.at(serverA) = RebootId{3};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must not be called again";

    guards.clear();
    ASSERT_EQ(1, numCalled)
        << "Callback must not be called when guards are destroyed";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(1, numCalled) << "Callback must not be called during destruction";
}

// Tests that callbacks and interleaved updates don't interfere
TEST_F(RebootTrackerTest, call_interleaved) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{1}}};

  uint64_t numCalled = 0;
  auto callback = [&numCalled]() { ++numCalled; };

  {
    RebootTracker rebootTracker{&scheduler};
    std::vector<CallbackGuard> guards{};
    CallbackGuard guard;

    // Set state to { serverA => 1 }
    rebootTracker.updateServerState(state);

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";

    // Set state to { serverA => 2 }
    state.at(serverA) = RebootId{2};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must be called after a change";

    // Set state to { serverA => 3 }
    state.at(serverA) = RebootId{3};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must not be called twice";

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{3}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";

    // Set state to { serverA => 4 }
    state.at(serverA) = RebootId{4};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must be called after a change";

    // Set state to { serverA => 5 }
    state.at(serverA) = RebootId{5};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "Callback must not be called twice";

    guards.clear();
    ASSERT_EQ(1, numCalled)
        << "Callback must not be called when guards are destroyed";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(1, numCalled) << "Callback must not be called during destruction";
}

// Tests that multiple callbacks and updates don't interfere
TEST_F(RebootTrackerTest, call_sequential) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{1}}};

  uint64_t numCalled = 0;
  auto callback = [&numCalled]() { ++numCalled; };

  {
    RebootTracker rebootTracker{&scheduler};
    std::vector<CallbackGuard> guards{};
    CallbackGuard guard;

    // Set state to { serverA => 1 }
    rebootTracker.updateServerState(state);

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         callback, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";

    // Set state to { serverA => 2 }
    state.at(serverA) = RebootId{2};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(2, numCalled) << "Both callbacks must be called after a change";

    // Set state to { serverA => 3 }
    state.at(serverA) = RebootId{3};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, numCalled) << "No callback must not be called twice";

    guards.clear();
    ASSERT_EQ(1, numCalled)
        << "Callback must not be called when guards are destroyed";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(1, numCalled) << "Callback must not be called during destruction";
}

// Test that a registered callback is removed when its guard is destroyed
TEST_F(RebootTrackerTest, guard_removes_callback) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{1}}};

  uint64_t counterA = 0;
  uint64_t counterB = 0;
  uint64_t counterC = 0;
  auto incrCounterA = [&counterA]() { ++counterA; };
  auto incrCounterB = [&counterB]() { ++counterB; };
  auto incrCounterC = [&counterC]() { ++counterC; };

  {
    RebootTracker rebootTracker{&scheduler};
    std::vector<CallbackGuard> guards{};
    CallbackGuard guard;

    // Set state to { serverA => 1 }
    rebootTracker.updateServerState(state);

    // Register callback
    guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                         incrCounterA, "");
    guards.emplace_back(std::move(guard));
    waitForSchedulerEmpty();
    ASSERT_EQ(0, counterA) << "Callback must not be called before a change";

    {
      // Register callback with a local guard
      auto localGuard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                                incrCounterB, "");
      waitForSchedulerEmpty();
      ASSERT_EQ(0, counterA) << "Callback must not be called before a change";
      ASSERT_EQ(0, counterB) << "Callback must not be called before a change";

      // Register callback
      guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                                incrCounterC, "");
      guards.emplace_back(std::move(guard));
      waitForSchedulerEmpty();
      ASSERT_EQ(0, counterA) << "Callback must not be called before a change";
      ASSERT_EQ(0, counterB) << "Callback must not be called before a change";
      ASSERT_EQ(0, counterC) << "Callback must not be called before a change";
    }
    waitForSchedulerEmpty();
    ASSERT_EQ(0, counterA) << "Callback must not be called when the guard is destroyed";
    ASSERT_EQ(0, counterB) << "Callback must not be called when the guard is destroyed";
    ASSERT_EQ(0, counterC) << "Callback must not be called when the guard is destroyed";

    // Set state to { serverA => 2 }
    state.at(serverA) = RebootId{2};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, counterA) << "Callback must be called after a change";
    ASSERT_EQ(0, counterB) << "Removed callback must not be called after a change";
    ASSERT_EQ(1, counterC) << "Callback must be called after a change";

    // Set state to { serverA => 3 }
    state.at(serverA) = RebootId{3};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(1, counterA) << "No callback must not be called twice";
    ASSERT_EQ(0, counterB) << "Removed callback must not be called after a change";
    ASSERT_EQ(1, counterC) << "No callback must not be called twice";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(0, counterA) << "Callback must not be called during destruction";
  ASSERT_EQ(0, counterB) << "Callback must not be called during destruction";
  ASSERT_EQ(0, counterC) << "Callback must not be called during destruction";
}

// Test that callback removed by a guard doesn't interfere with other registered
// callbacks for the same server and reboot id
TEST_F(RebootTrackerTest, guard_doesnt_interfere) {
  auto state = std::unordered_map<ServerID, RebootId>{{serverA, RebootId{1}}};

  uint64_t numCalled = 0;
  auto callback = [&numCalled]() { ++numCalled; };

  {
    RebootTracker rebootTracker{&scheduler};

    // Set state to { serverA => 1 }
    rebootTracker.updateServerState(state);

    {
      // Register callback
      auto guard = rebootTracker.callMeOnChange(PeerState{serverA, RebootId{1}},
                                                callback, "");
      waitForSchedulerEmpty();
      ASSERT_EQ(0, numCalled) << "Callback must not be called before a change";
    }
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled)
        << "Callback must not be called when the guard is destroyed";

    // Set state to { serverA => 2 }
    state.at(serverA) = RebootId{2};
    rebootTracker.updateServerState(state);
    waitForSchedulerEmpty();
    ASSERT_EQ(0, numCalled) << "Callback must not be called after a change "
                               "when the guard was destroyed before";
  }
  // RebootTracker was destroyed now

  waitForSchedulerEmpty();
  ASSERT_EQ(0, numCalled) << "Callback must not be called during destruction";
}
