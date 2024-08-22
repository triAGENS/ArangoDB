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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

// Must be included early to avoid
//   "explicit specialization of 'std::char_traits<unsigned char>' after
//   instantiation"
// errors.
#include "../3rdParty/iresearch/core/utils/string.hpp"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Replication2/Mocks/FakeAsyncExecutor.h"
#include "Replication2/Mocks/FakeFollowerFactory.h"
#include "Replication2/Mocks/FakeReplicatedState.h"
#include "Replication2/Mocks/FakeStorageEngineMethods.h"
#include "Replication2/Mocks/LeaderCommunicatorMock.h"
#include "Replication2/Mocks/MockVocbase.h"
#include "Replication2/Mocks/ParticipantsFactoryMock.h"
#include "Replication2/Mocks/ReplicatedLogMetricsMock.h"
#include "Replication2/Mocks/ReplicatedStateMetricsMock.h"
#include "Replication2/Mocks/SchedulerMocks.h"
#include "Replication2/Mocks/StorageEngineMethodsMock.h"
#include "Mocks/Servers.h"

#include "Replication2/ReplicatedLog/DefaultRebootIdCache.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "Replication2/ReplicatedState/ReplicatedStateImpl.tpp"
#include "Replication2/ReplicatedState/StateInterfaces.h"
#include "Replication2/ReplicatedState/StateInterfaces.tpp"
#include "Replication2/StateMachines/BlackHole/Monostate.h"
#include "Replication2/IScheduler.h"
#include "Replication2/Mocks/SchedulerMocks.h"
#include "Replication2/Mocks/RebootIdCacheMock.h"

#include <thread>

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::replicated_state;

namespace {
struct EmptyState {
  static constexpr std::string_view NAME = "empty-state";

  using LeaderType = test::EmptyLeaderType<EmptyState>;
  using FollowerType = test::EmptyFollowerType<EmptyState>;
  using EntryType = test::DefaultEntryType;
  using FactoryType = test::DefaultFactory<LeaderType, FollowerType>;
  using CoreType = test::TestCoreType;
  using CoreParameterType = void;
  using CleanupHandlerType = void;
  using MetadataType = Monostate;
};
struct FakeState {
  static constexpr std::string_view NAME = "fake-state";

  using LeaderType = test::FakeLeaderType<FakeState>;
  using FollowerType = test::FakeFollowerType<FakeState>;
  using EntryType = test::DefaultEntryType;
  using FactoryType = test::RecordingFactory<LeaderType, FollowerType>;
  using CoreType = test::TestCoreType;
  using CoreParameterType = void;
  using CleanupHandlerType = void;
  using MetadataType = Monostate;
};
}  // namespace

template<typename State>
struct StateManagerTest : testing::Test {
  GlobalLogIdentifier gid = GlobalLogIdentifier("db", LogId{1});
  arangodb::tests::mocks::MockServer mockServer =
      arangodb::tests::mocks::MockServer();
  replication2::tests::MockVocbase vocbaseMock =
      replication2::tests::MockVocbase(mockServer.server(),
                                       "documentStateMachineTestDb", 2);
  std::shared_ptr<storage::rocksdb::test::DelayedExecutor> executor =
      std::make_shared<storage::rocksdb::test::DelayedExecutor>();
  // Note that this purposefully does not initialize the PersistedStateInfo that
  // is returned by the StorageEngineMethods. readMetadata() will return a
  // document not found error unless you initialize it in your test.
  std::shared_ptr<storage::test::FakeStorageEngineMethodsContext>
      storageContext =
          std::make_shared<storage::test::FakeStorageEngineMethodsContext>(
              12, gid.id, executor, LogRange{});
  storage::IStorageEngineMethods* methodsPtr =
      storageContext->getMethods().release();
  std::shared_ptr<test::ReplicatedLogMetricsMock> logMetricsMock =
      std::make_shared<test::ReplicatedLogMetricsMock>();
  std::shared_ptr<replication2::tests::ReplicatedStateMetricsMock>
      stateMetricsMock =
          std::make_shared<replication2::tests::ReplicatedStateMetricsMock>(
              "foo");
  std::shared_ptr<ReplicatedLogGlobalSettings> optionsMock =
      std::make_shared<ReplicatedLogGlobalSettings>();
  LoggerContext loggerContext = LoggerContext(Logger::REPLICATION2);
  agency::ServerInstanceReference const myself = {"SELF", RebootId{1}};
  agency::ServerInstanceReference const other = {"OTHER", RebootId{1}};

  std::shared_ptr<test::DelayedScheduler> scheduler =
      std::make_shared<test::DelayedScheduler>();
  std::shared_ptr<test::DelayedScheduler> logScheduler =
      std::make_shared<test::DelayedScheduler>();
  std::shared_ptr<test::RebootIdCacheMock> rebootIdCache =
      std::make_shared<test::RebootIdCacheMock>();
  std::shared_ptr<test::FakeFollowerFactory> fakeFollowerFactory =
      std::make_shared<test::FakeFollowerFactory>();
  std::shared_ptr<DefaultParticipantsFactory> participantsFactory =
      std::make_shared<replicated_log::DefaultParticipantsFactory>(
          fakeFollowerFactory, logScheduler, rebootIdCache);

  std::shared_ptr<ReplicatedLog> log =
      std::make_shared<replicated_log::ReplicatedLog>(
          std::unique_ptr<storage::IStorageEngineMethods>{methodsPtr},
          logMetricsMock, optionsMock, participantsFactory, loggerContext,
          myself);

  LoggerContext const loggerCtx{Logger::REPLICATED_STATE};

  std::shared_ptr<typename State::FactoryType> stateFactory =
      std::make_shared<typename State::FactoryType>();
  std::unique_ptr<typename State::CoreType> stateCore =
      std::make_unique<typename State::CoreType>();
  std::shared_ptr<ReplicatedState<State>> state =
      std::make_shared<ReplicatedState<State>>(
          gid, log, stateFactory, loggerCtx, stateMetricsMock, scheduler);
  ReplicatedLogConnection connection =
      log->connect(state->createStateHandle(vocbaseMock, std::nullopt));
};

struct StateManagerTest_EmptyState : StateManagerTest<EmptyState> {};
struct StateManagerTest_FakeState : StateManagerTest<FakeState> {};

TEST_F(StateManagerTest_EmptyState, get_leader_state_machine_early) {
  // Overview:
  // - establish leadership
  // - let recovery finish
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.

  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kCompleted}};

  auto const term = agency::LogPlanTermSpecification{LogTerm{1}, myself};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};
  EXPECT_CALL(*rebootIdCache, getRebootIdsFor);
  std::ignore = log->updateConfig(term, config, myself);
  {
    auto logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kLeader);
    ASSERT_EQ(logStatus.localState, LocalStateMachineStatus::kUnconfigured);
  }
  auto const leader =
      std::dynamic_pointer_cast<ILogLeader>(log->getParticipant());
  ASSERT_NE(leader, nullptr);
  // Note that we have to check the (quick) status, rather than using
  // `waitForLeadership()`, because the futures are resolved asynchronously.
  // This means recovery might also already be completed at that point, but we
  // want to do some checks while leadership has been established, but before
  // recovery has completed.
  EXPECT_FALSE(log->getQuickStatus().leadershipEstablished);
  // TODO Do we want this loop for the test? On the one hand, it makes the test
  // more complex than strictly necessary.
  //      On the other hand, it makes it more robust to unrelated changes.
  EXPECT_TRUE(executor->hasWork() or scheduler->hasWork() or
              logScheduler->hasWork());
  bool runAtLeastOnce = false;
  EXPECT_CALL(*rebootIdCache, registerCallbackOnChange);
  for (auto status = log->getQuickStatus();
       not status.leadershipEstablished and
       (executor->hasWork() or scheduler->hasWork() or logScheduler->hasWork());
       status = log->getQuickStatus()) {
    runAtLeastOnce = true;
    // While leadership isn't established, the leader state manager isn't yet
    // instantiated.
    // TODO It would be nice if this would report kConnecting instead of
    //      kUnconfigured in this situation.
    EXPECT_EQ(status.localState, LocalStateMachineStatus::kUnconfigured);
    // the state machine must not be available until after recovery
    auto stateMachine = state->getLeader();
    EXPECT_EQ(stateMachine, nullptr);

    if (scheduler->hasWork()) {
      scheduler->runOnce();
    } else if (executor->hasWork()) {
      executor->runOnce();
    } else {
      logScheduler->runAll();
    }
  }
  EXPECT_TRUE(runAtLeastOnce);
  EXPECT_TRUE(scheduler->hasWork());
  EXPECT_FALSE(executor->hasWork());

  // Leadership was established, but recovery hasn't been completed. That means
  // the state machine must still be inaccessible.
  {
    auto status = log->getQuickStatus();
    EXPECT_EQ(status.localState, LocalStateMachineStatus::kRecovery);
    auto stateMachine = state->getLeader();
    EXPECT_EQ(stateMachine, nullptr);
  }
  scheduler->runOnce();
  // Now we should be operational.
  {
    EXPECT_EQ(log->getQuickStatus().localState,
              LocalStateMachineStatus::kOperational);
    EXPECT_NE(state->getLeader(), nullptr);
  }
  // Run the rest for completeness' sake.
  scheduler->runAll();
  // We should have converged to a stable state now.
  EXPECT_FALSE(scheduler->hasWork());
  EXPECT_FALSE(executor->hasWork());
  // Nothing should have changed:
  {
    EXPECT_EQ(log->getQuickStatus().localState,
              LocalStateMachineStatus::kOperational);
    EXPECT_NE(state->getLeader(), nullptr);
  }
}

TEST_F(StateManagerTest_EmptyState,
       get_follower_state_machine_early_with_snapshot_without_truncate) {
  // Overview:
  //  - start with a valid snapshot
  //  - send successful append entries request
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.
  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kCompleted}};

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};
  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_TRUE(logStatus.snapshotAvailable);

    // The state machine should be constructed, but without knowledge whether
    // the snapshot will be valid in the current term, and thus report
    // connecting.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    auto const termIndexPair = TermIndexPair(term, LogIndex(1));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  EXPECT_FALSE(scheduler->hasWork());
  EXPECT_TRUE(executor->hasWork());
  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  // An apply entries has been scheduled
  EXPECT_TRUE(logScheduler->hasWork());
  logScheduler->runAll();

  // We should have converged to a stable state now.
  EXPECT_FALSE(executor->hasWork());
  EXPECT_FALSE(logScheduler->hasWork());

  // The append entries response should be ready.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_TRUE(appendEntriesResponse.snapshotAvailable);
  }

  {
    // The state machine should now be operational and accessible.
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
    auto const stateMachine = state->getFollower();
    EXPECT_NE(stateMachine, nullptr);
  }

  scheduler->runAll();
}

TEST_F(
    StateManagerTest_EmptyState,
    get_follower_state_machine_early_with_snapshot_with_truncate_append_entries_before_snapshot) {
  // Overview:
  //  - start with a valid snapshot
  //  - send a truncating append entries request
  //  - process the append entries request
  //  - let the state machine acquire a snapshot
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.

  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kCompleted}};
  auto const leaderComm =
      std::make_shared<replication2::tests::LeaderCommunicatorMock>();
  fakeFollowerFactory->leaderComm = leaderComm;

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};
  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_TRUE(logStatus.snapshotAvailable);

    // The state machine should be constructed, but without knowledge whether
    // the snapshot will be valid in the current term, and thus report
    // unconfigured.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    // This should result in a truncate
    auto const termIndexPair = TermIndexPair(term, LogIndex(2));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    // Snapshot is now missing
    EXPECT_FALSE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  EXPECT_TRUE(executor->hasWork());
  EXPECT_TRUE(scheduler->hasWork());

  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  EXPECT_FALSE(executor->hasWork());
  logScheduler->runOnce();

  // The append entries response should be ready.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_FALSE(appendEntriesResponse.snapshotAvailable);
  }

  // The snapshot wasn't acquired yet, so the state machine must not be
  // accessible.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState,
              LocalStateMachineStatus::kAcquiringSnapshot);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  futures::Promise<Result> p;
  EXPECT_CALL(*leaderComm, reportSnapshotAvailable(MessageId{1}))
      .WillOnce([&](MessageId) { return p.getFuture(); });
  // Acquire the snapshot
  p.setValue(Result{});
  scheduler->runAll();
  // We should have converged to a stable state now.
  EXPECT_FALSE(logScheduler->hasWork());
  EXPECT_FALSE(executor->hasWork());

  {
    // The state machine should now be operational and accessible.
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
    auto const stateMachine = state->getFollower();
    EXPECT_NE(stateMachine, nullptr);
  }

  scheduler->runAll();
}

TEST_F(
    StateManagerTest_EmptyState,
    get_follower_state_machine_early_with_snapshot_with_truncate_append_entries_after_snapshot) {
  // Overview:
  //  - start with a valid snapshot
  //  - send a truncating append entries request, invalidating the snapshot
  //  - let the state machine acquire a snapshot
  //  - process the append entries request
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.

  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kCompleted}};
  auto const leaderComm =
      std::make_shared<replication2::tests::LeaderCommunicatorMock>();
  fakeFollowerFactory->leaderComm = leaderComm;

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};
  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_TRUE(logStatus.snapshotAvailable);

    // The state machine should be constructed, but without knowledge whether
    // the snapshot will be valid in the current term, and thus report
    // unconfigured.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    // This should result in a truncate
    auto const termIndexPair = TermIndexPair(term, LogIndex(2));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    // Snapshot is now missing due to log truncation
    EXPECT_FALSE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  EXPECT_TRUE(executor->hasWork());
  EXPECT_TRUE(scheduler->hasWork());

  futures::Promise<Result> p;
  EXPECT_CALL(*leaderComm, reportSnapshotAvailable(MessageId{1}))
      .WillOnce([&](MessageId) { return p.getFuture(); });
  // Acquire the snapshot
  p.setValue(Result{});
  scheduler->runOnce();

  // The snapshot was acquired, but the append entries request wasn't processed
  // successfully, so the state machine must not be accessible.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  // We should have converged to a stable state now.
  EXPECT_FALSE(executor->hasWork());
  logScheduler->runAll();

  // The append entries response should be ready.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_TRUE(appendEntriesResponse.snapshotAvailable);
  }

  {
    // The state machine should now be operational and accessible.
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
    auto const stateMachine = state->getFollower();
    EXPECT_NE(stateMachine, nullptr);
  }

  scheduler->runAll();
}

TEST_F(
    StateManagerTest_EmptyState,
    get_follower_state_machine_early_without_snapshot_append_entries_before_snapshot) {
  // Overview:
  //  - start without a snapshot
  //  - send successful append entries request
  //  - let the state machine acquire a snapshot
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.
  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kFailed}};
  auto const leaderComm =
      std::make_shared<replication2::tests::LeaderCommunicatorMock>();
  fakeFollowerFactory->leaderComm = leaderComm;

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};

  futures::Promise<Result> p;
  EXPECT_CALL(*leaderComm, reportSnapshotAvailable(MessageId{1}))
      .WillOnce([&](MessageId) { return p.getFuture(); });
  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_FALSE(logStatus.snapshotAvailable);

    // The state machine should be constructed, but is without a snapshot. It
    // must not be accessible and should return "connecting".
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    auto const termIndexPair = TermIndexPair(term, LogIndex(1));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  EXPECT_FALSE(executor->hasWork());
  logScheduler->runAll();

  // The append entries response should be ready.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_FALSE(appendEntriesResponse.snapshotAvailable);
  }

  // The snapshot wasn't acquired yet, so the state machine must not be
  // accessible.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState,
              LocalStateMachineStatus::kAcquiringSnapshot);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  // Acquire the snapshot
  p.setValue(Result{});
  scheduler->runAll();
  // We should have converged to a stable state now.
  EXPECT_FALSE(scheduler->hasWork());
  EXPECT_FALSE(executor->hasWork());

  {
    // The state machine should now be operational and accessible.
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
    auto const stateMachine = state->getFollower();
    EXPECT_NE(stateMachine, nullptr);
  }
}

TEST_F(
    StateManagerTest_EmptyState,
    get_follower_state_machine_early_without_snapshot_append_entries_after_snapshot) {
  // Overview:
  //  - start without a snapshot
  //  - let the state machine acquire a snapshot
  //  - send successful append entries request
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.
  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id, .snapshot = {.status = SnapshotStatus::kFailed}};
  auto const leaderComm =
      std::make_shared<replication2::tests::LeaderCommunicatorMock>();
  fakeFollowerFactory->leaderComm = leaderComm;

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};

  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_FALSE(logStatus.snapshotAvailable);

    // The state machine should be constructed, but is without a snapshot. It
    // must not be accessible and should return "connecting".
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  // The snapshot wasn't acquired, and neither have we got a successful append
  // entries request.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  futures::Promise<Result> p;
  EXPECT_CALL(*leaderComm, reportSnapshotAvailable(MessageId{0}))
      .WillOnce([&](MessageId) { return p.getFuture(); });

  // Acquire the snapshot
  p.setValue(Result{});
  scheduler->runOnce();

  // The snapshot was acquired, but we haven't seen a successful append entries
  // request yet, so don't know whether the snapshot will stay valid.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    auto const termIndexPair = TermIndexPair(term, LogIndex(1));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    // Should still be waiting whether the snapshot will be valid in the current
    // term.
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
    auto const stateMachine = state->getFollower();
    EXPECT_EQ(stateMachine, nullptr);
  }

  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  EXPECT_FALSE(executor->hasWork());
  logScheduler->runAll();

  // The append entries response should be ready.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_TRUE(appendEntriesResponse.snapshotAvailable);
  }

  {
    // The state machine should now be operational and accessible.
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
    auto const stateMachine = state->getFollower();
    EXPECT_NE(stateMachine, nullptr);
  }

  scheduler->runAll();
}

TEST_F(StateManagerTest_FakeState, follower_acquire_snapshot) {
  // Overview:
  //  - start without a snapshot
  //  - let the state machine acquire a snapshot, but respond with an error
  //  - let the state machine acquire a snapshot
  // Meanwhile check that the follower state machine is inaccessible until the
  // end.

  storageContext->meta = storage::PersistedStateInfo{
      .stateId = gid.id,
      .snapshot = {.status = SnapshotStatus::kUninitialized}};
  auto const leaderComm =
      std::make_shared<replication2::tests::LeaderCommunicatorMock>();
  fakeFollowerFactory->leaderComm = leaderComm;

  auto const term = LogTerm{1};
  auto const planTerm = agency::LogPlanTermSpecification{term, other};
  auto const config = agency::ParticipantsConfig{
      1, agency::ParticipantsFlagsMap{{myself.serverId, {}}},
      agency::LogPlanConfig{}};
  std::ignore = log->updateConfig(planTerm, config, myself);
  {
    auto const logStatus = log->getQuickStatus();
    ASSERT_EQ(logStatus.role, ParticipantRole::kFollower);
    EXPECT_FALSE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
  }

  auto const follower =
      std::dynamic_pointer_cast<ILogFollower>(log->getParticipant());
  ASSERT_NE(follower, nullptr);
  auto const leaderId = other.serverId;
  // Send an append entries request, just with the leader-establishing log
  // entry.
  auto appendEntriesFuture = std::invoke([&] {
    auto const waitForSync = true;
    auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = leaderId,
                                                 .participants = config};
    auto payload = LogMetaPayload{std::move(meta)};
    auto const termIndexPair = TermIndexPair(term, LogIndex(1));
    auto logEntry = InMemoryLogEntry(
        LogEntry(termIndexPair, std::move(payload)), waitForSync);
    auto request = AppendEntriesRequest(
        term, leaderId, TermIndexPair(LogTerm(0), LogIndex(0)), LogIndex(1),
        LogIndex(0), MessageId(1), waitForSync,
        AppendEntriesRequest::EntryContainer({std::move(logEntry)}));
    return follower->appendEntries(request);
  });
  EXPECT_FALSE(appendEntriesFuture.isReady());

  auto state = stateFactory->getLatestFollower();
  EXPECT_FALSE(state->apply.wasTriggered());
  EXPECT_TRUE(state->acquire.wasTriggered());

  // The append entries request has now been received, but not processed (i.e.
  // not written to disk) yet.
  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kConnecting);
  }

  // EXPECT_FALSE(scheduler->hasWork());
  EXPECT_TRUE(executor->hasWork());
  // Process the append entries request, i.e. write to disk.
  executor->runOnce();
  // An apply entries has been scheduled
  EXPECT_TRUE(logScheduler->hasWork());
  logScheduler->runAll();
  scheduler->runAll();

  // We should have converged to a stable state now.
  EXPECT_FALSE(executor->hasWork());
  EXPECT_FALSE(logScheduler->hasWork());

  // The append entries response should be ready, but there's no snapshot yet.
  {
    ASSERT_TRUE(appendEntriesFuture.isReady());
    ASSERT_TRUE(appendEntriesFuture.hasValue());
    auto const appendEntriesResponse = appendEntriesFuture.waitAndGet();
    EXPECT_TRUE(appendEntriesResponse.isSuccess())
        << appendEntriesResponse.errorCode;
    EXPECT_FALSE(appendEntriesResponse.snapshotAvailable);
  }

  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState,
              LocalStateMachineStatus::kAcquiringSnapshot);
  }

  EXPECT_TRUE(state->acquire.wasTriggered());
  EXPECT_FALSE(state->apply.wasTriggered());

  // Respond that the snapshot transfer has failed
  state->acquire.resolveWithAndReset(Result(TRI_ERROR_FAILED));
  EXPECT_FALSE(state->apply.wasTriggered());
  EXPECT_FALSE(state->acquire.wasTriggered());

  logScheduler->runAll();
  scheduler->runAll();

  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_FALSE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState,
              LocalStateMachineStatus::kAcquiringSnapshot);
  }

  EXPECT_TRUE(state->acquire.wasTriggered());

  auto p = futures::Promise<Result>();
  p.setValue(Result());
  EXPECT_CALL(*leaderComm, reportSnapshotAvailable(MessageId{1}))
      .WillOnce([&](MessageId) { return p.getFuture(); });

  state->acquire.resolveWithAndReset(Result());

  logScheduler->runAll();
  scheduler->runAll();

  EXPECT_FALSE(state->acquire.wasTriggered());

  {
    auto const logStatus = log->getQuickStatus();
    EXPECT_TRUE(logStatus.snapshotAvailable);
    EXPECT_EQ(logStatus.localState, LocalStateMachineStatus::kOperational);
  }
}
