////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include <gmock/gmock.h>

#include "Basics/voc-errors.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/LeaseManager.h"
#include "Cluster/LeaseManager/LeaseManagerNetworkHandler.h"
#include "Cluster/LeaseManager/LeasesReport.h"
#include "Cluster/LeaseManager/LeasesReportInspectors.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterTypes.h"
#include "Cluster/RebootTracker.h"
#include "IResearch/RestHandlerMock.h"
#include "Metrics/MetricsFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "Scheduler/SupervisedScheduler.h"

#include "Mocks/Servers.h"
#include "PreparedResponseConnectionPool.h"

// TODO: This has to be removed
#include "Inspection/VPack.h"

using namespace arangodb;
using namespace arangodb::cluster;
using namespace arangodb::tests;
using namespace arangodb::tests::mocks;

namespace {
auto emptyPrint() -> std::string { return "Dummy Details"; }
}  // namespace

struct MockLeaseManagerNetworkHandler : public ILeaseManagerNetworkHandler {
  MOCK_METHOD(futures::Future<Result>, abortIds,
              (ServerID const& server, std::vector<LeaseId> const& leasedFrom,
               std::vector<LeaseId> const& leasedTo),
              (const, noexcept, override));
  MOCK_METHOD(futures::Future<ManyServersLeasesReport>, collectFullLeaseReport,
              (), (const, noexcept, override));
  MOCK_METHOD(futures::Future<ManyServersLeasesReport>,
              collectLeaseReportForServer, (ServerID const& onlyShowServer),
              (const, noexcept, override));
};

class LeaseManagerTest : public ::testing::Test,
                         tests::LogSuppressor<Logger::CLUSTER, LogLevel::WARN> {
 protected:
  LeaseManagerTest()
      : mockApplicationServer(),
        scheduler(std::make_unique<SupervisedScheduler>(
            mockApplicationServer.server(), 2, 64, 128, 1024 * 1024, 4096, 4096,
            128, 0.0,
            std::make_shared<SchedulerMetrics>(
                mockApplicationServer.server()
                    .template getFeature<
                        arangodb::metrics::MetricsFeature>()))),
        rebootTracker(scheduler.get()),
        oldId{ServerState::instance()->getId()} {}

  MockRestServer mockApplicationServer;
  std::unique_ptr<SupervisedScheduler> scheduler;
  RebootTracker rebootTracker;
  ServerID const myId{"CRDN_TEST_1"};
  ServerID const oldId;

  static ServerID const serverA;
  static ServerID const serverB;
  static ServerID const serverC;

  containers::FlatHashMap<ServerID, ServerHealthState> state;

  // ApplicationServer needs to be prepared in order for the scheduler to start
  // threads.

  void SetUp() override {
    scheduler->start();
    ServerState::instance()->setId(myId);
    state = containers::FlatHashMap<ServerID, ServerHealthState>{
        {serverA, ServerHealthState{.rebootId = RebootId{1},
                                    .status = ServerHealth::kGood}},
        {serverB, ServerHealthState{.rebootId = RebootId{1},
                                    .status = ServerHealth::kGood}},
        {serverC, ServerHealthState{.rebootId = RebootId{1},
                                    .status = ServerHealth::kGood}}};
    rebootTracker.updateServerState(state);
  }
  void TearDown() override {
    /* NOTE:
     * If you ever see this test failing with such a message:
     * There was still a task queued by the LeaseManager and afterwards we did
     * not call `waitForSchedulerEmpty();` Please check the failing test if this
     * could be the case, e.g. has the test waited after a reboot of the server?
     * Has the test waited if handing in an Illegal PeerState?
     * 2024-04-05T08:55:47Z [2352775] WARNING {threads} Scheduler received
     * shutdown, but there are still tasks on the queue: jobsSubmitted=1
     * jobsDone=0 Signal: SIGSEGV (signal SIGSEGV: invalid address (fault
     * address: 0xf))
     */
    scheduler->shutdown();
    ServerState::instance()->setId(oldId);
  }

  bool schedulerEmpty() const {
    auto stats = scheduler->queueStatistics();

    return stats._queued == 0 && stats._working == 0;
  }

  void waitForSchedulerEmpty() const {
    while (!schedulerEmpty()) {
      std::this_thread::yield();
    }
  }

  void rebootServer(ServerID const& server) {
    auto it = state.find(server);
    ASSERT_NE(it, state.end())
        << "Test setup incorrect, tried to reboot a server that does not "
           "participate in the test.";
    it->second.rebootId = RebootId{it->second.rebootId.value() + 1};
    rebootTracker.updateServerState(state);
    // Need to wait for the scheduler to actually work on the RebootTracker.
    waitForSchedulerEmpty();
  }

  auto getPeerState(ServerID const& server) -> PeerState {
    auto it = state.find(server);
    TRI_ASSERT(it != state.end())
        << "Test setup incorrect, tried to getPeerState for a server that does "
           "not participate in the test.";

    return PeerState{.serverId = server, .rebootId = it->second.rebootId};
  }

  auto peerStateToJSONKey(const PeerState& peerState) {
    return fmt::format("{}:{}", peerState.serverId,
                       basics::StringUtils::itoa(peerState.rebootId.value()));
  }

  auto buildManager() -> LeaseManager {
    auto networkMock =
        std::make_unique<testing::NiceMock<MockLeaseManagerNetworkHandler>>();
    // Add default behaviour: Successfully abort all IDs.
    ON_CALL(*networkMock, abortIds).WillByDefault([&]() -> auto {
      auto promise = futures::Promise<Result>{};
      auto future = promise.getFuture();
      scheduler->queue(RequestLane::CONTINUATION,
                       [promise = std::move(promise)]() mutable -> void {
                         promise.setValue(Result{});
                       });
      return future;
    });
    return LeaseManager{rebootTracker, std::move(networkMock), *scheduler};
  }

  auto getNetworkMock(LeaseManager& manager)
      -> testing::NiceMock<MockLeaseManagerNetworkHandler>* {
    return static_cast<testing::NiceMock<MockLeaseManagerNetworkHandler>*>(
        manager.getNetworkHandler());
  }

  auto assertLeasedFromListContainsLease(VPackSlice leasesVPack,
                                         PeerState const& leaseIsFor,
                                         LeaseId const& leaseId) {
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(myId));
    leasesVPack = leasesVPack.get(myId);
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey("leasedFromRemote"));
    leasesVPack = leasesVPack.get("leasedFromRemote");
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(peerStateToJSONKey(leaseIsFor)))
        << "LeaseManager should have an entry for the server "
        << peerStateToJSONKey(leaseIsFor)
        << " full list: " << leasesVPack.toJson();
    auto leaseList = leasesVPack.get(peerStateToJSONKey(leaseIsFor));
    ASSERT_TRUE(leaseList.isArray());
    bool foundEntry = false;
    for (auto const& it : VPackArrayIterator(leaseList)) {
      ASSERT_TRUE(it.isString());
      auto view = it.stringView();
      if (view.starts_with(basics::StringUtils::itoa(leaseId.id()) + " -> ")) {
        foundEntry = true;
        break;
      }
    }
    EXPECT_TRUE(foundEntry)
        << "LeaseManager should have an entry for the lease " << leaseId
        << " full list: " << leaseList.toJson();
  }

  auto assertLeasedFromListContainsLease(ManyServersLeasesReport manyReport,
                                         PeerState const& leaseIsFor,
                                         LeaseId const& leaseId) {
    // TODO Implement this, remove VPackVersion
    auto builder = velocypack::serialize(manyReport);
    assertLeasedFromListContainsLease(builder.slice(), leaseIsFor, leaseId);
  }

  auto assertLeasedFromListDoesNotContainLease(VPackSlice leasesVPack,
                                               PeerState const& leaseIsFor,
                                               LeaseId const& leaseId) {
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(myId));
    leasesVPack = leasesVPack.get(myId);
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey("leasedFromRemote"));
    leasesVPack = leasesVPack.get("leasedFromRemote");
    ASSERT_TRUE(leasesVPack.isObject());
    if (auto leaseList = leasesVPack.get(peerStateToJSONKey(leaseIsFor));
        !leaseList.isNone()) {
      ASSERT_TRUE(leaseList.isArray());
      bool foundEntry = false;
      for (auto const& it : VPackArrayIterator(leaseList)) {
        ASSERT_TRUE(it.isString());
        auto view = it.stringView();
        if (view.starts_with(basics::StringUtils::itoa(leaseId.id()) +
                             " -> ")) {
          foundEntry = true;
          break;
        }
      }
      EXPECT_FALSE(foundEntry)
          << "LeaseManager should not have an entry for the lease " << leaseId
          << " full list: " << leaseList.toJson();
    }
    // Else case okay if we have no entry for the server, we cannot have an
    // entry for the lease.
  }

  auto assertLeasedFromListDoesNotContainLease(
      ManyServersLeasesReport manyReport, PeerState const& leaseIsFor,
      LeaseId const& leaseId) {
    // TODO Implement this, remove VPackVersion
    auto builder = velocypack::serialize(manyReport);
    assertLeasedFromListDoesNotContainLease(builder.slice(), leaseIsFor,
                                            leaseId);
  }

  auto assertLeasedToListContainsLease(VPackSlice leasesVPack,
                                       PeerState const& leaseIsTo,
                                       LeaseId const& leaseId) {
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(myId));
    leasesVPack = leasesVPack.get(myId);
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey("leasedToRemote"));
    leasesVPack = leasesVPack.get("leasedToRemote");
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(peerStateToJSONKey(leaseIsTo)))
        << "LeaseManager should have an entry for the server "
        << peerStateToJSONKey(leaseIsTo)
        << " full list: " << leasesVPack.toJson();
    auto leaseList = leasesVPack.get(peerStateToJSONKey(leaseIsTo));
    ASSERT_TRUE(leaseList.isArray());
    bool foundEntry = false;
    for (auto const& it : VPackArrayIterator(leaseList)) {
      ASSERT_TRUE(it.isString());
      auto view = it.stringView();
      if (view.starts_with(basics::StringUtils::itoa(leaseId.id()) + " -> ")) {
        foundEntry = true;
        break;
      }
    }
    EXPECT_TRUE(foundEntry)
        << "LeaseManager should have an entry for the lease " << leaseId
        << " full list: " << leaseList.toJson();
  }

  auto assertLeasedToListContainsLease(ManyServersLeasesReport manyReport,
                                       PeerState const& leaseIsTo,
                                       LeaseId const& leaseId) {
    // TODO Implement this, remove VPackVersion
    auto builder = velocypack::serialize(manyReport);
    assertLeasedToListContainsLease(builder.slice(), leaseIsTo, leaseId);
  }

  auto assertLeasedToListDoesNotContainLease(VPackSlice leasesVPack,
                                             PeerState const& leaseIsTo,
                                             LeaseId const& leaseId) {
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey(myId));
    leasesVPack = leasesVPack.get(myId);
    ASSERT_TRUE(leasesVPack.isObject());
    ASSERT_TRUE(leasesVPack.hasKey("leasedToRemote"));
    leasesVPack = leasesVPack.get("leasedToRemote");
    ASSERT_TRUE(leasesVPack.isObject());
    if (auto leaseList = leasesVPack.get(peerStateToJSONKey(leaseIsTo));
        !leaseList.isNone()) {
      ASSERT_TRUE(leaseList.isArray());
      bool foundEntry = false;
      for (auto const& it : VPackArrayIterator(leaseList)) {
        ASSERT_TRUE(it.isString());
        auto view = it.stringView();
        if (view.starts_with(basics::StringUtils::itoa(leaseId.id()) +
                             " -> ")) {
          foundEntry = true;
          break;
        }
      }
      EXPECT_FALSE(foundEntry)
          << "LeaseManager should not have an entry for the lease " << leaseId
          << " full list: " << leaseList.toJson();
    }
    // Else case okay if we have no entry for the server, we cannot have an
    // entry for the lease.
  }

  auto assertLeasedToListDoesNotContainLease(ManyServersLeasesReport manyReport,
                                             PeerState const& leaseIsTo,
                                             LeaseId const& leaseId) {
    // TODO Implement this, remove VPackVersion
    auto builder = velocypack::serialize(manyReport);
    assertLeasedToListDoesNotContainLease(builder.slice(), leaseIsTo, leaseId);
  }
};

ServerID const LeaseManagerTest::serverA = "PRMR-srv-A";
ServerID const LeaseManagerTest::serverB = "PRMR-srv-B";
ServerID const LeaseManagerTest::serverC = "PRMR-srv-C";

TEST_F(LeaseManagerTest, test_every_lease_has_a_unique_id) {
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  auto ignoreMe = []() noexcept -> void {};
  auto guardOne = leaseManager.requireLease(leaseIsFor, ::emptyPrint, ignoreMe);
  auto guardTwo = leaseManager.requireLease(leaseIsFor, ::emptyPrint, ignoreMe);
  EXPECT_NE(guardOne.id(), guardTwo.id());
  auto leaseReport =
      leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
  assertLeasedFromListContainsLease(leaseReport, leaseIsFor, guardOne.id());
  assertLeasedFromListContainsLease(leaseReport, leaseIsFor, guardTwo.id());
}

TEST_F(LeaseManagerTest, test_a_lease_from_remote_can_be_moved_around) {
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  uint64_t wasCalled = 0;
  auto countingCallback = [&]() noexcept -> void { wasCalled++; };
  auto networkMock = getNetworkMock(leaseManager);
  // We are not allowed to abort remote leases here.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  struct MyStructure {
    LeaseManager::LeaseFromRemoteGuard lease;
  };
  {
    auto guardOne =
        leaseManager.requireLease(leaseIsFor, ::emptyPrint, countingCallback);
    auto storedId = guardOne.id();
    auto myStructure = MyStructure{.lease = std::move(guardOne)};
    waitForSchedulerEmpty();
    EXPECT_EQ(wasCalled, 0) << "Callback was called while moving around.";
    // We now go out of scope with myStruct. Can call abort now.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{storedId},
                                       std::vector<LeaseId>{}))
        .Times(1);
  }
}

TEST_F(LeaseManagerTest, test_handout_lease_is_not_directly_destroyed) {
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  uint8_t wasCalled = 0;
  auto ignoreMe = [&wasCalled]() noexcept -> void { wasCalled++; };
  auto networkMock = getNetworkMock(leaseManager);
  // We are not allowed to abort remote leases here.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  {
    auto leaseId = LeaseId{42};
    auto guardOne =
        leaseManager.handoutLease(leaseIsFor, leaseId, ::emptyPrint, ignoreMe);
    ASSERT_TRUE(guardOne.ok()) << "Failed to handout a lease with given ID";
    EXPECT_EQ(guardOne->id(), leaseId) << "LeaseId should be the same.";
    waitForSchedulerEmpty();
    EXPECT_EQ(wasCalled, 0) << "The guard is still inside the result. Callback "
                               "is not allowed to be called";

    // We now go out of scope. Can call abort now.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{leaseId}))
        .Times(1);
  }
  EXPECT_EQ(wasCalled, 0)
      << "We have now locally lost the lease, should not call abort";
}

TEST_F(LeaseManagerTest,
       test_cannot_handout_same_lease_id_twice_for_same_peer) {
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  auto leaseIsFor = getPeerState(serverA);
  uint8_t wasCalled = 0;
  auto ignoreMe = [&wasCalled]() noexcept -> void { wasCalled++; };
  // We are not allowed to abort remote leases here.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  {
    auto leaseId = LeaseId{42};
    auto guardOne =
        leaseManager.handoutLease(leaseIsFor, leaseId, ::emptyPrint, ignoreMe);
    ASSERT_TRUE(guardOne.ok()) << "Failed to handout a lease with given ID";
    EXPECT_EQ(guardOne->id(), leaseId) << "LeaseId should be the same.";
    auto guardTwo =
        leaseManager.handoutLease(leaseIsFor, leaseId, ::emptyPrint, ignoreMe);
    EXPECT_FALSE(guardTwo.ok())
        << "Should not be able to handout the same lease ID twice.";
    EXPECT_EQ(wasCalled, 0)
        << "One of the abort callbacks triggered, should not happen.";

    // We now go out of scope. Can call abort now.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{leaseId}))
        .Times(1);
  }
  EXPECT_EQ(wasCalled, 0)
      << "One of the abort callbacks triggered, should not happen, one fails "
         "to be created, the other goes out of scope";
}
TEST_F(LeaseManagerTest, test_can_handout_different_lease_id_for_same_peer) {
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  auto leaseIsFor = getPeerState(serverA);
  uint8_t wasCalled = 0;
  auto ignoreMe = [&wasCalled]() noexcept -> void { wasCalled++; };
  // We are not allowed to abort remote leases here.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  {
    auto leaseId = LeaseId{42};
    auto guardOne =
        leaseManager.handoutLease(leaseIsFor, leaseId, ::emptyPrint, ignoreMe);
    ASSERT_TRUE(guardOne.ok()) << "Failed to handout a lease with given ID";
    EXPECT_EQ(guardOne->id(), leaseId) << "LeaseId should be the same.";
    auto leaseIdTwo = LeaseId{1337};
    auto guardTwo = leaseManager.handoutLease(leaseIsFor, leaseIdTwo,
                                              ::emptyPrint, ignoreMe);
    ASSERT_TRUE(guardTwo.ok())
        << "Failed to handout a second lease with a different ID";
    EXPECT_EQ(guardTwo->id(), leaseIdTwo) << "LeaseId should be the same.";

    EXPECT_EQ(wasCalled, 0)
        << "One of the abort callbacks triggered, should not happen.";

#if 0
    // TODO: As soon as we move the "abort" to a background thread, it should be
    // called only once. We now go out of scope. Can call abort now. NOTE: The
    // ordering on the Vector is not guaranteed. If this ever fails it is safe
    // to replace the EXPECT_CALL with a shouldOnce. That matches all possible
    // orders of the vector
    EXPECT_CALL(*networkMock,
                abortIds(serverA, std::vector<LeaseId>{leaseId, leaseIdTwo}))
        .Times(1);
#else
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{leaseId}))
        .Times(1);
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{leaseIdTwo}))
        .Times(1);
#endif
  }
  EXPECT_EQ(wasCalled, 0)
      << "One of the abort callbacks triggered, should not happen";
}

TEST_F(LeaseManagerTest, test_a_lease_to_remote_can_be_moved_around) {
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  uint64_t wasCalled = 0;
  auto countingCallback = [&]() noexcept -> void { wasCalled++; };
  auto networkMock = getNetworkMock(leaseManager);
  // We are not allowed to abort remote leases here.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  struct MyStructure {
    LeaseManager::LeaseToRemoteGuard lease;
  };
  {
    auto storedId = LeaseId{1337};
    auto guardOne = leaseManager.handoutLease(leaseIsFor, storedId,
                                              ::emptyPrint, countingCallback);
    ASSERT_TRUE(guardOne.ok());
    auto myStructure = MyStructure{.lease = std::move(guardOne.get())};
    waitForSchedulerEmpty();
    EXPECT_EQ(wasCalled, 0) << "Callback was called while moving around.";
    // We now go out of scope with myStruct. Can call abort now.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{storedId}))
        .Times(1);
  }
}

TEST_F(LeaseManagerTest,
       test_can_handout_same_lease_id_twice_for_different_peers) {
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  auto leaseIsForOther = getPeerState(serverB);
  auto ignoreMe = []() noexcept -> void {};
  auto leaseId = LeaseId{42};
  auto guardOne =
      leaseManager.handoutLease(leaseIsFor, leaseId, ::emptyPrint, ignoreMe);
  auto guardTwo = leaseManager.handoutLease(leaseIsForOther, leaseId,
                                            ::emptyPrint, ignoreMe);
}

TEST_F(LeaseManagerTest, test_lease_is_removed_from_list_on_guard_destruction) {
  bool rebootCallbackCalled = false;
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  auto leaseIsFor = getPeerState(serverA);
  LeaseId storedId;
  {
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto lease = leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);
    storedId = lease.id();
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedFromListContainsLease(leaseReport, leaseIsFor, lease.id());
    // Prepare to be called to abort this ID.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{storedId},
                                       std::vector<LeaseId>{}))
        .Times(1);
  }
  {
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor, storedId);
  }
  // Need to wait for the scheduler to actually work on the RebootTracker.
  waitForSchedulerEmpty();
  // We locally lost the lease, we should not call the onLeaseLost callback.
  EXPECT_FALSE(rebootCallbackCalled);
}

TEST_F(LeaseManagerTest,
       test_lease_to_remote_is_removed_from_list_on_guard_destruction) {
  bool rebootCallbackCalled = false;
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  auto leaseIsTo = getPeerState(serverA);
  LeaseId storedId{42};
  {
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto lease =
        leaseManager.handoutLease(leaseIsTo, storedId, ::emptyPrint, callback);
    ASSERT_TRUE(lease.ok())
        << "Failed to handout a lease with given ID: " << lease.errorMessage();

    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListContainsLease(leaseReport, leaseIsTo, lease->id());
    // Prepare to be called to abort this ID.
    EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{},
                                       std::vector<LeaseId>{storedId}))
        .Times(1);
  }
  {
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListDoesNotContainLease(leaseReport, leaseIsTo, storedId);
  }
  // Need to wait for the scheduler to actually work on the RebootTracker.
  waitForSchedulerEmpty();
  // We locally lost the lease, we should not call the onLeaseLost callback.
  EXPECT_FALSE(rebootCallbackCalled);
}

TEST_F(LeaseManagerTest, test_lease_from_remote_can_cancel_abort_callback) {
  bool rebootCallbackCalled = false;
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  LeaseId storedId;
  {
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto lease = leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);
    storedId = lease.id();
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedFromListContainsLease(leaseReport, leaseIsFor, lease.id());
    lease.cancel();
  }
  {
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor, storedId);
  }
  // Need to wait for the scheduler to actually work on the RebootTracker.
  // (Should not do anything here)
  waitForSchedulerEmpty();
  // We locally lost the lease, we should not call the onLeaseLost callback.
  // So completing the lease does not actually change anything here.
  EXPECT_FALSE(rebootCallbackCalled);
}

TEST_F(LeaseManagerTest, test_lease_to_remote_can_cancel_abort_callback) {
  bool rebootCallbackCalled = false;
  auto leaseManager = buildManager();
  auto leaseIsFor = getPeerState(serverA);
  LeaseId storedId{42};
  {
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto lease =
        leaseManager.handoutLease(leaseIsFor, storedId, ::emptyPrint, callback);
    ASSERT_TRUE(lease.ok())
        << "Failed to handout a lease with given ID: " << lease.errorMessage();
    storedId = lease->id();
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListContainsLease(leaseReport, leaseIsFor, lease->id());
    lease->cancel();
  }
  {
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor, storedId);
  }
  // Need to wait for the scheduler to actually work on the RebootTracker.
  // (Should not do anything here)
  waitForSchedulerEmpty();
  // We locally lost the lease, we should not call the onLeaseLost callback.
  // So completing the lease does not actually change anything here.
  EXPECT_FALSE(rebootCallbackCalled);
}

TEST_F(LeaseManagerTest, test_lease_from_remote_is_aborted_on_peer_reboot) {
  {
    bool rebootCallbackCalled = false;
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    [[maybe_unused]] auto lease =
        leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);
    rebootServer(serverA);
    // After a reboot of the other server, the onLeaseAbort callback should be
    // triggered
    EXPECT_TRUE(rebootCallbackCalled);
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor,
                                            lease.id());
  }
}

TEST_F(LeaseManagerTest, test_lease_to_remote_is_aborted_on_peer_reboot) {
  {
    bool rebootCallbackCalled = false;
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    LeaseId storedId{42};
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto lease =
        leaseManager.handoutLease(leaseIsFor, storedId, ::emptyPrint, callback);
    ASSERT_TRUE(lease.ok())
        << "Failed to handout a lease with given ID: " << lease.errorMessage();
    rebootServer(serverA);
    // After a reboot of the other server, the onLeaseAbort callback should be
    // triggered
    EXPECT_TRUE(rebootCallbackCalled);
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor, lease->id());
  }
}

TEST_F(LeaseManagerTest,
       test_canceled_lease_from_remote_is_not_aborted_on_peer_reboot) {
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    [[maybe_unused]] auto lease =
        leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);
    lease.cancel();
    {
      // Cancel does take the Lease out of the list!
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor,
                                              lease.id());
    }
    rebootServer(serverA);
    // NOTE: Lease is still in Scope, but the callback should not be called.
    {
      // Rebooting the server does not magically add the lease to the list
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor,
                                              lease.id());
    }
    EXPECT_FALSE(rebootCallbackCalled)
        << "Called callback on canceled lease if server rebooted";
  }
  EXPECT_FALSE(rebootCallbackCalled)
      << "Called callback on canceled lease if guard goes out of scope";
}

TEST_F(LeaseManagerTest,
       test_canceled_lease_to_remote_is_not_aborted_on_peer_reboot) {
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };
    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    LeaseId id{42};
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto lease =
        leaseManager.handoutLease(leaseIsFor, id, ::emptyPrint, callback);
    ASSERT_TRUE(lease.ok())
        << "Failed to handout a lease with given ID: " << lease.errorMessage();
    lease->cancel();
    {
      // Cancel does take the Lease out of the list!
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor,
                                            lease->id());
    }
    rebootServer(serverA);
    // NOTE: Lease is still in Scope, but the callback should not be called.
    {
      // Rebooting the server does not magically add the lease to the list
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor,
                                            lease->id());
    }
    EXPECT_FALSE(rebootCallbackCalled)
        << "Called callback on canceled lease if server rebooted";
  }
  EXPECT_FALSE(rebootCallbackCalled)
      << "Called callback on canceled lease if guard goes out of scope";
}

TEST_F(LeaseManagerTest, test_acquire_lease_for_rebooted_server) {
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };

    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);

    rebootServer(serverA);
    // Now ServerA is rebooted and the peerState is outdated.
    ASSERT_LT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, server was not rebooted.";

    auto lease = leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);

    // Requiring a least for an outdated peerState should actually trigger the
    // RebootTracker to intervene.
    waitForSchedulerEmpty();
    {
      // This situation is handled the same as if reboot would be AFTER
      // getting the lease. So Server should be dropped here.
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor,
                                              lease.id());
    }
    EXPECT_TRUE(rebootCallbackCalled)
        << "We registered a lease for a dead server. We need to get called.";
  }
}

TEST_F(LeaseManagerTest, test_handout_lease_for_rebooted_server) {
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };

    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    LeaseId id{42};

    rebootServer(serverA);
    // Now ServerA is rebooted and the peerState is outdated.
    ASSERT_LT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, server was not rebooted.";

    auto lease =
        leaseManager.handoutLease(leaseIsFor, id, ::emptyPrint, callback);

    // Requiring a least for an outdated peerState should actually trigger the
    // RebootTracker to intervene.
    waitForSchedulerEmpty();
    {
      // This situation is handled the same as if reboot would be AFTER
      // getting the lease. So Server should be dropped here.
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor, id);
    }
    EXPECT_TRUE(rebootCallbackCalled)
        << "We registered a lease for a dead server. We need to get called.";
  }
}

TEST_F(LeaseManagerTest, test_acquire_lease_for_server_with_newer_reboot_id) {
  // NOTE: This can happen in production in the following way:
  // 1. Server is rebooted, Local State is updated.
  // 2. RebootTracker schedules the Handling Callbacks.
  // 3. The caller now LooksUp the Local State and gets the new RebootId.
  // 4. The caller now tries to acquire a Lease for the new RebootId.
  // 5. Only now the handling Callbacks scheduled in 2. are executed.
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };

    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    leaseIsFor.rebootId = RebootId{leaseIsFor.rebootId.value() + 1};

    // Now ServerA is rebooted, the RebootTracker has not yet handled it.
    ASSERT_GT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, lease is not ahead of RebootTracker.";

    auto lease = leaseManager.requireLease(leaseIsFor, ::emptyPrint, callback);

    // Give RebootTracker time to intervene.
    waitForSchedulerEmpty();

    EXPECT_FALSE(rebootCallbackCalled)
        << "We are ahead of the RebootTracker. So we should not get aborted.";
    {
      // Lease should be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListContainsLease(leaseReport, leaseIsFor, lease.id());
    }

    // Now move RebootTracker to new state, it now sees sme id as the leaser.
    rebootServer(serverA);

    ASSERT_EQ(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, RebootIds should now be aligned.";

    EXPECT_FALSE(rebootCallbackCalled)
        << "We are ahead of the RebootTracker. So we should not get aborted.";
    {
      // Lease should be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListContainsLease(leaseReport, leaseIsFor, lease.id());
    }

    // Reboot once more. Now we should be behind the RebootTracker. Causing the
    // callback to be called.
    rebootServer(serverA);

    ASSERT_LT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, RebootId of Tracker should now be ahead of "
           "Lease.";

    EXPECT_TRUE(rebootCallbackCalled)
        << "Now the reboot tracker has overtaken us, we need to be aborted.";
    {
      // This situation is handled the same as if reboot would be AFTER
      // getting the lease. So Server should be dropped here.
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsFor,
                                              lease.id());
    }
  }
}

TEST_F(LeaseManagerTest, test_handout_lease_for_server_with_newer_reboot_id) {
  // NOTE: This can happen in production in the following way:
  // 1. A peer server reboots, and then sends out a require lease request
  // 2. The server running the manager receives the request, before the reboot
  // tracker has updated the state.
  // 3. Then it has to handout the lease, for a seemingly newer server version.
  bool rebootCallbackCalled = false;
  {
    auto callback = [&]() noexcept { rebootCallbackCalled = true; };

    auto leaseManager = buildManager();
    auto leaseIsFor = getPeerState(serverA);
    leaseIsFor.rebootId = RebootId{leaseIsFor.rebootId.value() + 1};

    // Now ServerA is rebooted, the RebootTracker has not yet handled it.
    ASSERT_GT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, lease is not ahead of RebootTracker.";

    LeaseId id{42};
    auto lease =
        leaseManager.handoutLease(leaseIsFor, id, ::emptyPrint, callback);
    ASSERT_TRUE(lease.ok())
        << "Failed to handout a lease with given ID: " << lease.errorMessage();

    // Give RebootTracker time to intervene.
    waitForSchedulerEmpty();

    EXPECT_FALSE(rebootCallbackCalled)
        << "We are ahead of the RebootTracker. So we should not get aborted.";
    {
      // Lease should be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListContainsLease(leaseReport, leaseIsFor, lease->id());
    }

    // Now move RebootTracker to new state, it now sees sme id as the leaser.
    rebootServer(serverA);

    ASSERT_EQ(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, RebootIds should now be aligned.";

    EXPECT_FALSE(rebootCallbackCalled)
        << "We are ahead of the RebootTracker. So we should not get aborted.";
    {
      // Lease should be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListContainsLease(leaseReport, leaseIsFor, lease->id());
    }

    // Reboot once more. Now we should be behind the RebootTracker. Causing the
    // callback to be called.
    rebootServer(serverA);

    ASSERT_LT(leaseIsFor.rebootId, state.find(serverA)->second.rebootId)
        << "Test setup incorrect, RebootId of Tracker should now be ahead of "
           "Lease.";

    EXPECT_TRUE(rebootCallbackCalled)
        << "Now the reboot tracker has overtaken us, we need to be aborted.";
    {
      // This situation is handled the same as if reboot would be AFTER
      // getting the lease. So Server should be dropped here.
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsFor,
                                            lease->id());
    }
  }
}

TEST_F(LeaseManagerTest, test_abort_given_leases_for_server_on_demand) {
  // This is the "abort" call used by the RestHandler

  /// 8 Leases, 4 for each handout and requested.
  /// 2 for Server A
  /// 1 for Server A, not to be erased
  /// 1 for Server B, not to be erased

  bool leaseOneForAToBeAborted = false;
  bool leaseTwoForAToBeAborted = false;
  bool leaseForAToBeKept = false;
  bool leaseBToBeKept = false;
  bool handoutLeaseOneForAToBeAborted = false;
  bool handoutLeaseTwoForAToBeAborted = false;
  bool handoutLeaseForAToBeKept = false;
  bool handoutLeaseBToBeKept = false;

  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  // The remote server tells us to Abort its leases.
  // We should not call it back.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  {
    auto leaseIsForA = getPeerState(serverA);
    auto leaseIsForB = getPeerState(serverB);

    LeaseId idAOne{42};
    LeaseId idATwo{43};
    LeaseId idAThree{44};
    LeaseId idBOne{42};
    EXPECT_EQ(idAOne, idBOne) << "Test setup incorrect, on purpose picked the "
                                 "same LeaseID for two servers";
    auto handoutLeaseAOne = leaseManager.handoutLease(
        leaseIsForA, idAOne, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(handoutLeaseOneForAToBeAborted)
              << "Aborted the same Lease twice";
          handoutLeaseOneForAToBeAborted = true;
        });
    auto handoutLeaseATwo = leaseManager.handoutLease(
        leaseIsForA, idATwo, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(handoutLeaseTwoForAToBeAborted)
              << "Aborted the same Lease twice";
          handoutLeaseTwoForAToBeAborted = true;
        });
    auto handoutLeaseAThree = leaseManager.handoutLease(
        leaseIsForA, idAThree, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(true) << "Called a callback that was not to be aborted";
          handoutLeaseForAToBeKept = true;
        });
    auto handoutLeaseBOne = leaseManager.handoutLease(
        leaseIsForB, idBOne, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(true) << "Called a callback that was not to be aborted";
          handoutLeaseBToBeKept = true;
        });

    auto leaseAOne =
        leaseManager.requireLease(leaseIsForA, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(leaseOneForAToBeAborted)
              << "Aborted the same Lease twice";
          leaseOneForAToBeAborted = true;
        });
    auto leaseATwo =
        leaseManager.requireLease(leaseIsForA, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(leaseTwoForAToBeAborted)
              << "Aborted the same Lease twice";
          leaseTwoForAToBeAborted = true;
        });
    auto leaseAThree =
        leaseManager.requireLease(leaseIsForA, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(true) << "Called a callback that was not to be aborted";
          leaseForAToBeKept = true;
        });
    auto leaseBOne =
        leaseManager.requireLease(leaseIsForB, ::emptyPrint, [&]() noexcept {
          EXPECT_FALSE(true) << "Called a callback that was not to be aborted";
          leaseBToBeKept = true;
        });

    // Give RebootTracker time to intervene.
    waitForSchedulerEmpty();

    EXPECT_FALSE(leaseOneForAToBeAborted)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(leaseTwoForAToBeAborted)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(leaseForAToBeKept)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(leaseBToBeKept)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(handoutLeaseOneForAToBeAborted)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(handoutLeaseTwoForAToBeAborted)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(handoutLeaseForAToBeKept)
        << "Lease aborted before we triggered the abort call.";
    EXPECT_FALSE(handoutLeaseBToBeKept)
        << "Lease aborted before we triggered the abort call.";

    {
      // Leases should all be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);

      assertLeasedFromListContainsLease(leaseReport, leaseIsForA,
                                        leaseAOne.id());
      assertLeasedFromListContainsLease(leaseReport, leaseIsForA,
                                        leaseATwo.id());
      assertLeasedFromListContainsLease(leaseReport, leaseIsForA,
                                        leaseAThree.id());
      assertLeasedFromListContainsLease(leaseReport, leaseIsForB,
                                        leaseBOne.id());

      assertLeasedToListContainsLease(leaseReport, leaseIsForA, idAOne);
      assertLeasedToListContainsLease(leaseReport, leaseIsForA, idATwo);
      assertLeasedToListContainsLease(leaseReport, leaseIsForA, idAThree);
      assertLeasedToListContainsLease(leaseReport, leaseIsForB, idBOne);
    }

    // Now the Actual test...
    // Abort some LeaseIDs for ServerA

    leaseManager.abortLeasesForServer(
        {.server = leaseIsForA,
         .leasedFrom = {leaseAOne.id(), leaseATwo.id()},
         .leasedTo = {idAOne, idATwo}});

    // TODO: Should Network be called?

    // Give RebootTracker time to work on the leases.
    waitForSchedulerEmpty();

    EXPECT_TRUE(leaseOneForAToBeAborted)
        << "Lease not aborted after the abort call.";
    EXPECT_TRUE(leaseTwoForAToBeAborted)
        << "Lease not aborted after the abort call.";
    EXPECT_TRUE(handoutLeaseOneForAToBeAborted)
        << "Lease not aborted after the abort call.";
    EXPECT_TRUE(handoutLeaseTwoForAToBeAborted)
        << "Lease not aborted after the abort call.";

    EXPECT_FALSE(leaseForAToBeKept) << "Lease falsely aborted by abort call.";
    EXPECT_FALSE(leaseBToBeKept) << "Lease falsely aborted by abort call.";
    EXPECT_FALSE(handoutLeaseForAToBeKept)
        << "Lease falsely aborted by abort call.";
    EXPECT_FALSE(handoutLeaseBToBeKept)
        << "Lease falsely aborted by abort call.";

    {
      // Leases should all be in the Report:
      auto leaseReport =
          leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsForA,
                                              leaseAOne.id());
      assertLeasedFromListDoesNotContainLease(leaseReport, leaseIsForA,
                                              leaseATwo.id());
      assertLeasedFromListContainsLease(leaseReport, leaseIsForA,
                                        leaseAThree.id());
      assertLeasedFromListContainsLease(leaseReport, leaseIsForB,
                                        leaseBOne.id());

      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsForA, idAOne);
      assertLeasedToListDoesNotContainLease(leaseReport, leaseIsForA, idATwo);
      assertLeasedToListContainsLease(leaseReport, leaseIsForA, idAThree);
      assertLeasedToListContainsLease(leaseReport, leaseIsForB, idBOne);
    }

    // Cancel all leases that survived the abort call.
    leaseAThree.cancel();
    leaseBOne.cancel();
    handoutLeaseAThree->cancel();
    handoutLeaseBOne->cancel();

    // Closing the Scope now, once for the Network Mock to validate
    // that nothing is to be cleaned up
    // and once to make sure we do not twice abort anything.
  }

  waitForSchedulerEmpty();

  EXPECT_TRUE(leaseOneForAToBeAborted)
      << "Lease not aborted after the abort call.";
  EXPECT_TRUE(leaseTwoForAToBeAborted)
      << "Lease not aborted after the abort call.";
  EXPECT_TRUE(handoutLeaseOneForAToBeAborted)
      << "Lease not aborted after the abort call.";
  EXPECT_TRUE(handoutLeaseTwoForAToBeAborted)
      << "Lease not aborted after the abort call.";

  EXPECT_FALSE(leaseForAToBeKept) << "Lease falsely aborted by abort call.";
  EXPECT_FALSE(leaseBToBeKept) << "Lease falsely aborted by abort call.";
  EXPECT_FALSE(handoutLeaseForAToBeKept)
      << "Lease falsely aborted by abort call.";
  EXPECT_FALSE(handoutLeaseBToBeKept) << "Lease falsely aborted by abort call.";
}

TEST_F(LeaseManagerTest, test_abort_with_same_id_for_lease_from_and_handout) {
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  // The remote server tells us to Abort its leases.
  // We should not call it back.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);

  for (bool abortHandedOut : {false, true}) {
    bool leaseHandedOut = false;
    bool leaseFromRemote = false;

    {
      auto leaseIsForA = getPeerState(serverA);
      auto guardForLeasedFrom =
          leaseManager.requireLease(leaseIsForA, ::emptyPrint, [&]() noexcept {
            EXPECT_FALSE(leaseFromRemote) << "Aborted the same Lease twice";
            leaseFromRemote = true;
          });
      auto guardForLeasedTo = leaseManager.handoutLease(
          leaseIsForA, guardForLeasedFrom.id(), ::emptyPrint, [&]() noexcept {
            EXPECT_FALSE(leaseHandedOut) << "Aborted the same Lease twice";
            leaseHandedOut = true;
          });
      ASSERT_TRUE(guardForLeasedTo.ok());
      ASSERT_EQ(guardForLeasedFrom.id(), guardForLeasedTo->id())
          << "Test setup incorrect. We test here that our peer selected the "
             "same "
             "ID to lease from us, as we did to lease from them";

      // Those are actually two tests, they test once if A is aborted, B stays,
      // and vice versa
      if (abortHandedOut) {
        leaseManager.abortLeasesForServer(
            {.server = leaseIsForA,
             .leasedFrom = {},
             .leasedTo = {LeaseId{guardForLeasedTo->id()}}});

        waitForSchedulerEmpty();

        EXPECT_TRUE(leaseHandedOut)
            << "The remotely aborted lease was not called";
        EXPECT_FALSE(leaseFromRemote)
            << "The not remotely aborted lease was called";

        guardForLeasedFrom.cancel();
      } else {
        leaseManager.abortLeasesForServer(
            {.server = leaseIsForA,
             .leasedFrom = {LeaseId{guardForLeasedFrom.id()}},
             .leasedTo = {}});

        waitForSchedulerEmpty();

        EXPECT_TRUE(leaseFromRemote)
            << "The remotely aborted lease was not called";
        EXPECT_FALSE(leaseHandedOut)
            << "The not remotely aborted lease was called";

        guardForLeasedTo->cancel();
      }
    }
  }
}

TEST_F(LeaseManagerTest, test_abort_before_register_race) {
  auto leaseManager = buildManager();
  auto networkMock = getNetworkMock(leaseManager);
  // The remote server tells us to Abort its leases.
  // We should not call it back.
  EXPECT_CALL(*networkMock, abortIds(testing::_, testing::_, testing::_))
      .Times(0);
  auto leaseIsForA = getPeerState(serverA);
  bool leaseHandoutCallbackCalled = false;

  // This situation can only happen on the handout side.
  // We simulate that the Caller wants a lease from this server.
  // But before we work on this request and actually register the Lease
  // the Caller already wants to abort it, e.g. because it got a reboot
  // from a different server.
  {
    auto id = LeaseId{42};
    // We need to hold the lease until the end of the scope.
    // otherwise the destructor callback might be lost.
    auto callback = [&]() noexcept -> void {
      EXPECT_FALSE(true) << "Callback should not be called.";
      leaseHandoutCallbackCalled = true;
    };

    // Now we abort the lease before we actually registered it.
    leaseManager.abortLeasesForServer(
        {.server = leaseIsForA, .leasedFrom = {}, .leasedTo = {id}});

    // Give RebootTracker time to intervene.
    // Note: We do this to make sure that the NetworkMock is not called.
    waitForSchedulerEmpty();

    // The Lease should not be in the Report:
    auto leaseReport =
        leaseManager.reportLeases(LeaseManager::GetType::LOCAL, std::nullopt);
    assertLeasedToListDoesNotContainLease(leaseReport, leaseIsForA, id);

    // Now try to get the lease.
    auto leaseGuard =
        leaseManager.handoutLease(leaseIsForA, id, ::emptyPrint, callback);
    ASSERT_FALSE(leaseGuard.ok())
        << "We got a lease, that was already aborted.";
  }
  EXPECT_FALSE(leaseHandoutCallbackCalled)
      << "Called the callback for the lease that was already aborted.";
}

// TODO Add tests with Multiple Servers.

#if 0
// Example imeplementation to create a bad response on the networkMock.
EXPECT_CALL(*networkMock, abortIds(serverA, std::vector<LeaseId>{storedId})).WillOnce([&]() -> auto {
  auto promise = futures::Promise<Result>{};
  auto future = promise.getFuture();
  scheduler->queue(RequestLane::CONTINUATION, [promise = std::move(promise)]() mutable -> void {
    promise.setValue(Result{TRI_ERROR_HTTP_NOT_FOUND});
  });
  return future;
});
#endif