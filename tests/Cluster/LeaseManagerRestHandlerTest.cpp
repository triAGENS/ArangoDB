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

#include "IResearch/RestHandlerMock.h"
#include "Mocks/Servers.h"

#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/AbortLeaseInformationInspector.h"
#include "Cluster/LeaseManager/LeaseManagerFeature.h"
#include "Cluster/LeaseManager/LeaseManagerRestHandler.h"
#include "Cluster/LeaseManager/LeasesReport.h"
#include "Metrics/MetricsFeature.h"
#include "Scheduler/SupervisedScheduler.h"

using namespace arangodb;
using namespace arangodb::cluster;

namespace {
auto emptyPrint() -> std::string { return "Dummy Details"; }

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

}  // namespace

class LeaseManagerRestHandlerTest : public ::testing::Test {
  struct LeaseResponse {
    VPackSlice leasedFromRemote;
    VPackSlice leasedToRemote;
  };

 protected:
  LeaseManagerRestHandlerTest()
      : server{},
        scheduler(std::make_unique<SupervisedScheduler>(
            server.server(), 2, 64, 128, 1024 * 1024, 4096, 4096, 128, 0.0,
            std::make_shared<SchedulerMetrics>(
                server.server()
                    .template getFeature<
                        arangodb::metrics::MetricsFeature>()))),
        rebootTracker(scheduler.get()),
        oldId{ServerState::instance()->getId()} {}

  tests::mocks::MockRestServer server;
  std::unique_ptr<SupervisedScheduler> scheduler;
  RebootTracker rebootTracker;
  ServerID const myId{"CRDN_TEST_1"};
  ServerID const oldId;

  static ServerID const serverA;
  static ServerID const serverB;
  static ServerID const serverC;

  containers::FlatHashMap<ServerID, ServerHealthState> state;

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

  auto extractResultBody(VPackSlice response) -> LeaseResponse {
    EXPECT_TRUE(response.isObject()) << "Did not respond with an object";

    EXPECT_EQ(basics::VelocyPackHelper::getNumericValue(response, "code", 42),
              static_cast<int>(rest::ResponseCode::OK));
    EXPECT_FALSE(response.hasKey("errorNum"));
    EXPECT_FALSE(response.hasKey("errorMessage"));
    EXPECT_TRUE(response.hasKey("error"));
    EXPECT_TRUE(response.get("error").isBool());
    EXPECT_FALSE(response.get("error").getBool());

    auto result = response.get("result");
    EXPECT_TRUE(result.isObject())
        << "Did not respond with a result entry of type object";
    EXPECT_TRUE(result.hasKey(myId));
    result = result.get(myId);
    EXPECT_TRUE(result.isObject())
        << "Did not respond with a result entry of type object for this server";

    auto leasedFromRemote = result.get("leasedFromRemote");
    EXPECT_TRUE(leasedFromRemote.isObject())
        << "Did not respond with a leasedFromRemote entry of type object";
    auto leasedToRemote = result.get("leasedToRemote");
    EXPECT_TRUE(leasedToRemote.isObject())
        << "Did not respond with a leasedToRemote entry of type object";
    return {.leasedFromRemote = leasedFromRemote,
            .leasedToRemote = leasedToRemote};
  }

  auto assertResponseIsNotFound(VPackSlice response) {
    ASSERT_TRUE(response.isObject()) << "Did not respond with an object";
    EXPECT_EQ(basics::VelocyPackHelper::getNumericValue(response, "code", 42),
              TRI_ERROR_HTTP_NOT_FOUND.value());
    EXPECT_EQ(
        basics::VelocyPackHelper::getNumericValue(response, "errorNum", 42),
        TRI_ERROR_HTTP_NOT_FOUND.value());
    EXPECT_TRUE(response.hasKey("errorMessage"));
    EXPECT_TRUE(response.get("errorMessage").isString());
    EXPECT_TRUE(response.hasKey("error"));
    EXPECT_TRUE(response.get("error").isBool());
    EXPECT_TRUE(response.get("error").getBool());
  }

  auto getPeerState(ServerID const& peerName) -> PeerState {
    auto it = state.find(peerName);
    TRI_ASSERT(it != state.end())
        << "Test setup incorrect, tried to getPeerState for a server that does "
           "not participate in the test.";

    return PeerState{.serverId = peerName, .rebootId = it->second.rebootId};
  }

  auto peerStateToJSONKey(const PeerState& peerState) {
    return fmt::format("{}:{}", peerState.serverId,
                       basics::StringUtils::itoa(peerState.rebootId.value()));
  }
};

ServerID const LeaseManagerRestHandlerTest::serverA = "PRMR-srv-A";
ServerID const LeaseManagerRestHandlerTest::serverB = "PRMR-srv-B";
ServerID const LeaseManagerRestHandlerTest::serverC = "PRMR-srv-C";

TEST_F(LeaseManagerRestHandlerTest, test_get_request) {
  auto& vocbase = server.getSystemDatabase();
  auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
  auto fakeResponse = std::make_unique<GeneralResponseMock>();
  auto resp = fakeResponse.get();
  fakeRequest->setRequestType(arangodb::rest::RequestType::GET);
  auto mgr = buildManager();

  LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                 fakeResponse.release(), &mgr);
  auto res = testee.execute();
  EXPECT_EQ(res, RestStatus::DONE);
  EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());
  EXPECT_EQ(resp->responseCode(), ResponseCode::OK);
  auto response = resp->_payload.slice();
  auto result = extractResultBody(response);
  EXPECT_TRUE(result.leasedToRemote.isEmptyObject())
      << "Leased to Remote is not empty";
  EXPECT_TRUE(result.leasedFromRemote.isEmptyObject())
      << "Leased From Remote is not empty";
}

TEST_F(LeaseManagerRestHandlerTest, test_get_request_including_leases) {
  auto& vocbase = server.getSystemDatabase();
  auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
  auto fakeResponse = std::make_unique<GeneralResponseMock>();
  auto resp = fakeResponse.get();
  fakeRequest->setRequestType(arangodb::rest::RequestType::GET);

  auto leaseManager = buildManager();

  LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                 fakeResponse.release(), &leaseManager);
  auto leaseIsForA = getPeerState(serverA);
  auto leaseGuardA =
      leaseManager.requireLease(leaseIsForA, ::emptyPrint, []() noexcept {});

  auto leaseIsForB = getPeerState(serverB);
  auto leaseGuardB1 =
      leaseManager.requireLease(leaseIsForB, ::emptyPrint, []() noexcept {});
  auto leaseGuardB2 =
      leaseManager.requireLease(leaseIsForB, ::emptyPrint, []() noexcept {});
  {
    ScopeGuard freeAllTheLeases{[&]() noexcept {
      // Make sure we cancel the leases before they go
      // out of scope, so we avoid trying to inform remote
      // peers
      leaseGuardA.cancel();
      leaseGuardB1.cancel();
      leaseGuardB2.cancel();
    }};
    auto res = testee.execute();
    EXPECT_EQ(res, RestStatus::DONE);
    EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());
    auto response = resp->_payload.slice();
    auto result = extractResultBody(response);
    EXPECT_TRUE(result.leasedToRemote.isEmptyObject())
        << "Leased to Remote is not empty";
    EXPECT_FALSE(result.leasedFromRemote.isEmptyObject())
        << "Leased From Remote is empty";
    EXPECT_TRUE(
        result.leasedFromRemote.hasKey(peerStateToJSONKey(leaseIsForA)));
    {
      auto forA = result.leasedFromRemote.get(peerStateToJSONKey(leaseIsForA));
      EXPECT_TRUE(forA.isArray());
      EXPECT_EQ(forA.length(), 1);
    }
    EXPECT_TRUE(
        result.leasedFromRemote.hasKey(peerStateToJSONKey(leaseIsForB)));
    {
      auto forB = result.leasedFromRemote.get(peerStateToJSONKey(leaseIsForB));
      EXPECT_TRUE(forB.isArray());
      EXPECT_EQ(forB.length(), 2);
    }
  }
}

TEST_F(LeaseManagerRestHandlerTest,
       test_get_request_including_leases_some_removed) {
  auto& vocbase = server.getSystemDatabase();
  auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
  auto fakeResponse = std::make_unique<GeneralResponseMock>();
  auto resp = fakeResponse.get();
  fakeRequest->setRequestType(arangodb::rest::RequestType::GET);

  auto leaseManager = buildManager();
  LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                 fakeResponse.release(), &leaseManager);
  auto leaseIsForA = getPeerState(serverA);
  auto leaseGuardA =
      leaseManager.requireLease(leaseIsForA, ::emptyPrint, []() noexcept {});

  auto leaseIsForB = getPeerState(serverB);
  auto leaseGuardB1 =
      leaseManager.requireLease(leaseIsForB, ::emptyPrint, []() noexcept {});
  auto leaseGuardB2 =
      leaseManager.requireLease(leaseIsForB, ::emptyPrint, []() noexcept {});

  // Cancel some leases. They should not be reported anymore.
  leaseGuardA.cancel();
  leaseGuardB1.cancel();
  {
    ScopeGuard freeAllTheLeases{[&]() noexcept {
      // Make sure we cancel the leases before they go
      // out of scope, so we avoid trying to inform remote
      // peers
      leaseGuardA.cancel();
      leaseGuardB1.cancel();
      leaseGuardB2.cancel();
    }};
    auto res = testee.execute();
    EXPECT_EQ(res, RestStatus::DONE);
    EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());
    auto response = resp->_payload.slice();
    auto result = extractResultBody(response);
    EXPECT_TRUE(result.leasedToRemote.isEmptyObject())
        << "Leased to Remote is not empty";
    EXPECT_FALSE(result.leasedFromRemote.isEmptyObject())
        << "Leased From Remote is empty";
    EXPECT_TRUE(
        result.leasedFromRemote.hasKey(peerStateToJSONKey(leaseIsForA)));
    {
      auto forA = result.leasedFromRemote.get(peerStateToJSONKey(leaseIsForA));
      EXPECT_TRUE(forA.isArray());
      EXPECT_EQ(forA.length(), 0);
    }
    EXPECT_TRUE(
        result.leasedFromRemote.hasKey(peerStateToJSONKey(leaseIsForB)));
    {
      auto forB = result.leasedFromRemote.get(peerStateToJSONKey(leaseIsForB));
      EXPECT_TRUE(forB.isArray());
      EXPECT_EQ(forB.length(), 1);
    }
  }
}

TEST_F(LeaseManagerRestHandlerTest, test_put_request) {
  auto& vocbase = server.getSystemDatabase();
  auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
  auto fakeResponse = std::make_unique<GeneralResponseMock>();
  auto resp = fakeResponse.get();
  fakeRequest->setRequestType(arangodb::rest::RequestType::PUT);

  auto leaseManager = buildManager();
  LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                 fakeResponse.release(), &leaseManager);
  auto res = testee.execute();
  EXPECT_EQ(res, RestStatus::DONE);
  EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());
  EXPECT_EQ(resp->responseCode(), ResponseCode::NOT_FOUND);
  auto response = resp->_payload.slice();
  assertResponseIsNotFound(response);
}

TEST_F(LeaseManagerRestHandlerTest, test_post_request) {
  auto& vocbase = server.getSystemDatabase();
  auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
  auto fakeResponse = std::make_unique<GeneralResponseMock>();
  auto resp = fakeResponse.get();
  fakeRequest->setRequestType(arangodb::rest::RequestType::POST);

  auto leaseManager = buildManager();
  LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                 fakeResponse.release(), &leaseManager);
  auto res = testee.execute();
  EXPECT_EQ(res, RestStatus::DONE);
  EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());
  EXPECT_EQ(resp->responseCode(), ResponseCode::NOT_FOUND);
  auto response = resp->_payload.slice();
  assertResponseIsNotFound(response);
}

TEST_F(LeaseManagerRestHandlerTest, test_delete_request) {
  bool calledOnAbortForA = false;
  bool calledOnAbortForB1 = false;
  bool calledOnAbortForB2 = false;

  auto leaseManager = buildManager();

  // Create some leases we can destroy
  auto leaseIsForA = getPeerState(serverA);
  auto leaseGuardA = leaseManager.requireLease(
      leaseIsForA, ::emptyPrint,
      [&calledOnAbortForA]() noexcept { calledOnAbortForA = true; });

  auto leaseIsForB = getPeerState(serverB);
  auto leaseGuardB1 = leaseManager.requireLease(
      leaseIsForB, ::emptyPrint,
      [&calledOnAbortForB1]() noexcept { calledOnAbortForB1 = true; });
  auto leaseGuardB2 = leaseManager.requireLease(
      leaseIsForB, ::emptyPrint,
      [&calledOnAbortForB2]() noexcept { calledOnAbortForB2 = true; });
  {
    ScopeGuard freeAllTheLeases{[&]() noexcept {
      // Make sure we cancel the leases before they go
      // out of scope, so we avoid trying to inform remote
      // peers
      leaseGuardA.cancel();
      leaseGuardB1.cancel();
      leaseGuardB2.cancel();
    }};

    AbortLeaseInformation abortInfo;
    abortInfo.server = leaseIsForB;
    abortInfo.leasedFrom.emplace_back(leaseGuardB1.id());

    auto& vocbase = server.getSystemDatabase();
    auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
    velocypack::serialize(fakeRequest->_payload, abortInfo);

    auto fakeResponse = std::make_unique<GeneralResponseMock>();
    auto resp = fakeResponse.get();
    fakeRequest->setRequestType(arangodb::rest::RequestType::DELETE_REQ);
    LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                   fakeResponse.release(), &leaseManager);
    auto res = testee.execute();
    EXPECT_EQ(res, RestStatus::DONE);
    EXPECT_EQ(RequestLane::CLIENT_FAST, testee.lane());

    waitForSchedulerEmpty();

    // Make sure our desired abort callback is hit
    EXPECT_FALSE(calledOnAbortForA)
        << "Aborted Server A, which was not triggered";
    EXPECT_TRUE(calledOnAbortForB1)
        << "Did not abort for Server B, first lease";
    EXPECT_FALSE(calledOnAbortForB2)
        << "Aborted for Server B, second lease, which was not triggered";

    // Assert we always return 200.
    auto response = resp->_payload.slice();
    EXPECT_TRUE(response.isObject());
    EXPECT_EQ(basics::VelocyPackHelper::getNumericValue(response, "code", 1337),
              static_cast<int>(rest::ResponseCode::OK));
    EXPECT_FALSE(
        basics::VelocyPackHelper::getBooleanValue(response, "error", true));
  }
}

TEST_F(LeaseManagerRestHandlerTest, test_delete_request_malformed) {
  auto leaseManager = buildManager();

  {
    auto& vocbase = server.getSystemDatabase();
    auto fakeRequest = std::make_unique<GeneralRequestMock>(vocbase);
    VPackBuilder malformedInput;
    {
      VPackObjectBuilder guard(&malformedInput);
      // This is partially correct, but not complete, should be rejected.
      malformedInput.add("server", VPackValue(serverB));
    }

    fakeRequest->_payload = std::move(malformedInput);

    auto fakeResponse = std::make_unique<GeneralResponseMock>();
    fakeRequest->setRequestType(arangodb::rest::RequestType::DELETE_REQ);
    LeaseManagerRestHandler testee(server.server(), fakeRequest.release(),
                                   fakeResponse.release(), &leaseManager);
    try {
      std::ignore = testee.execute();
      ASSERT_FALSE(true) << "Rest handler should have thrown an exception"
                         << " because the input was malformed.";
    } catch (arangodb::basics::Exception const& e) {
      EXPECT_EQ(e.code(), TRI_ERROR_DESERIALIZE);
    }
  }
}
/// TODO: Add authentication tests!