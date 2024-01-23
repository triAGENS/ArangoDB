////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/ReplicatedLog/Components/LogFollower.h"
#include "Replication2/Mocks/FakeStorageEngineMethods.h"
#include "Replication2/Mocks/FakeAsyncExecutor.h"
#include "Replication2/Mocks/ReplicatedLogMetricsMock.h"
#include "Replication2/Mocks/SchedulerMocks.h"
#include "Replication2/ReplicatedState/StateStatus.h"
#include <immer/flex_vector_transient.hpp>

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::replicated_state;

namespace {
struct ReplicatedStateHandleMock : IReplicatedStateHandle {
  MOCK_METHOD(std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>,
              resignCurrentState, (), (noexcept, override));
  MOCK_METHOD(void, leadershipEstablished,
              (std::unique_ptr<IReplicatedLogLeaderMethods>), (override));
  MOCK_METHOD(void, becomeFollower,
              (std::unique_ptr<IReplicatedLogFollowerMethods>), (override));
  MOCK_METHOD(void, acquireSnapshot, (ServerID leader, LogIndex, std::uint64_t),
              (noexcept, override));
  MOCK_METHOD(void, updateCommitIndex, (LogIndex), (noexcept, override));
  MOCK_METHOD(replicated_state::Status, getInternalStatus, (),
              (const, override));
};

auto generateEntries(LogTerm term, LogRange range)
    -> AppendEntriesRequest::EntryContainer {
  auto tr = AppendEntriesRequest::EntryContainer::transient_type{};
  for (auto idx : range) {
    tr.push_back(InMemoryLogEntry{
        LogEntry{term, idx, LogPayload::createFromString("foo")}});
  }
  return tr.persistent();
}

}  // namespace

struct AppendEntriesFollowerTest : ::testing::Test {
  std::uint64_t const objectId = 1;
  LogId const logId = LogId{12};
  std::shared_ptr<storage::rocksdb::test::SyncExecutor> executor =
      std::make_shared<storage::rocksdb::test::SyncExecutor>();
  std::shared_ptr<test::SyncScheduler> scheduler =
      std::make_shared<test::SyncScheduler>();

  std::shared_ptr<storage::test::FakeStorageEngineMethodsContext> storage;

  std::shared_ptr<ReplicatedLogGlobalSettings> options =
      std::make_shared<ReplicatedLogGlobalSettings>();
  std::shared_ptr<FollowerTermInformation> termInfo =
      std::make_shared<FollowerTermInformation>();

  ReplicatedStateHandleMock* stateHandle = new ReplicatedStateHandleMock();
  std::shared_ptr<ReplicatedLogMetrics> metrics =
      std::make_shared<test::ReplicatedLogMetricsMock>();

  MessageId lastMessageId{1};

  auto makeFollowerManager() {
    return std::make_shared<FollowerManager>(
        storage->getMethods(),
        std::unique_ptr<ReplicatedStateHandleMock>(stateHandle), termInfo,
        options, metrics, nullptr, scheduler,
        LoggerContext{Logger::REPLICATION2});
  }

  void SetUp() override {
    storage = std::make_shared<storage::test::FakeStorageEngineMethodsContext>(
        storage::test::FakeStorageEngineMethodsContext{
            objectId, logId, executor, LogRange{LogIndex{1}, LogIndex{100}},
            storage::PersistedStateInfo{
                .stateId = logId,
                .snapshot = {.status = SnapshotStatus::kCompleted,
                             .timestamp = {},
                             .error = {}},
                .generation = {},
                .specification = {}}});
  }
};

TEST_F(AppendEntriesFollowerTest, append_entries_with_commit_index) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) {
        EXPECT_NE(newMethods, nullptr);
        methods = std::move(newMethods);
      });

  auto follower = makeFollowerManager();

  EXPECT_CALL(*stateHandle, updateCommitIndex(LogIndex{50}));

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{0};
    request.leaderCommit = LogIndex{50};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();

    EXPECT_TRUE(result.isSuccess()) << result.reason.getErrorMessage();
  }
}

TEST_F(AppendEntriesFollowerTest, append_entries_fail_wrong_term) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  auto follower = makeFollowerManager();

  EXPECT_CALL(*stateHandle, updateCommitIndex).Times(0);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{0};
    request.leaderCommit = LogIndex{50};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{2};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.reason.error,
              AppendEntriesErrorReason::ErrorType::kWrongTerm);
  }
}

TEST_F(AppendEntriesFollowerTest, append_entries_fail_wrong_leader) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  auto follower = makeFollowerManager();

  EXPECT_CALL(*stateHandle, updateCommitIndex).Times(0);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{0};
    request.leaderCommit = LogIndex{50};
    request.leaderId = "INVALID";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.reason.error,
              AppendEntriesErrorReason::ErrorType::kInvalidLeaderId);
  }
}

TEST_F(AppendEntriesFollowerTest, append_entries_no_match) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  auto follower = makeFollowerManager();

  EXPECT_CALL(*stateHandle, updateCommitIndex).Times(0);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{0};
    request.leaderCommit = LogIndex{50};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{2}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.reason.error,
              AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch);
  }
}

TEST_F(AppendEntriesFollowerTest, append_entries_update_syncIndex) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  auto follower = makeFollowerManager();
  methods->releaseIndex(LogIndex{50});  // allow compaction upto 50

  EXPECT_CALL(*stateHandle, updateCommitIndex(LogIndex{55})).Times(1);

  auto oldMessageId = ++lastMessageId;

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{40};  // compaction up to 40
    request.leaderCommit = LogIndex{55};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    request.entries =
        generateEntries(LogTerm{1}, LogRange{LogIndex{100}, LogIndex{120}});
    auto result = follower->appendEntries(request).get();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(result.syncIndex, LogIndex{119});
  }

  EXPECT_EQ(storage->log.begin()->first, LogIndex{40});    // compacted
  EXPECT_EQ(storage->log.rbegin()->first, LogIndex{119});  // [40, 120)

  // We report the correct syncIndex even in case of an append entries error
  {
    AppendEntriesRequest request;
    request.messageId = oldMessageId;
    request.lowestIndexToKeep = LogIndex{40};
    request.leaderCommit = LogIndex{55};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{119}};
    request.entries =
        generateEntries(LogTerm{1}, LogRange{LogIndex{230}, LogIndex{240}});
    auto result = follower->appendEntries(request).get();
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.syncIndex, LogIndex{119});
  }
}

TEST_F(AppendEntriesFollowerTest, append_entries_trigger_compaction) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  auto follower = makeFollowerManager();
  methods->releaseIndex(LogIndex{50});  // allow compaction upto 50

  EXPECT_CALL(*stateHandle, updateCommitIndex(LogIndex{50})).Times(1);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{50};  // allow compaction upto 50
    request.leaderCommit = LogIndex{50};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();

    EXPECT_TRUE(result.isSuccess());
  }

  // we expect compaction to happen
  ASSERT_FALSE(storage->log.empty());
  EXPECT_EQ(storage->log.begin()->first, LogIndex{50});
}

TEST_F(AppendEntriesFollowerTest, append_entries_trigger_snapshot) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{1};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  auto follower = makeFollowerManager();
  methods->releaseIndex(LogIndex{50});  // allow compaction upto 50

  // updateCommitIndex must not be called without a snapshot
  EXPECT_CALL(*stateHandle, updateCommitIndex).Times(0);
  EXPECT_CALL(*stateHandle, acquireSnapshot("leader", testing::_, 1)).Times(1);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{200};  // allow compaction upto 50
    request.leaderCommit = LogIndex{240};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{1};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.entries =
        generateEntries(LogTerm{1}, LogRange{LogIndex{200}, LogIndex{250}});
    auto result = follower->appendEntries(request).get();
    EXPECT_TRUE(result.isSuccess());
  }

  // we expect the snapshot to be invalidated
  // and the log truncated
  ASSERT_FALSE(storage->log.empty());
  EXPECT_EQ(storage->log.begin()->first, LogIndex{200});
  ASSERT_TRUE(storage->meta.has_value());
  EXPECT_EQ(storage->meta->snapshot.status, SnapshotStatus::kInvalidated);
}

TEST_F(AppendEntriesFollowerTest, append_entries_rewrite) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{2};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  auto follower = makeFollowerManager();
  methods->releaseIndex(LogIndex{50});  // allow compaction upto 50

  EXPECT_CALL(*stateHandle, updateCommitIndex(LogIndex{55})).Times(1);
  EXPECT_CALL(*stateHandle, acquireSnapshot).Times(0);

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{40};  // compaction up to 40
    request.leaderCommit = LogIndex{55};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{2};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{50}};
    request.entries =
        generateEntries(LogTerm{2}, LogRange{LogIndex{51}, LogIndex{60}});
    auto result = follower->appendEntries(request).get();
    EXPECT_TRUE(result.isSuccess());
  }

  // we expect the snapshot to be invalidated
  // and the log truncated
  ASSERT_FALSE(storage->log.empty());
  EXPECT_EQ(storage->log.begin()->first, LogIndex{40});   // compacted
  EXPECT_EQ(storage->log.rbegin()->first, LogIndex{59});  // [40, 60)
}

TEST_F(AppendEntriesFollowerTest, outdated_message_id) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{2};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  auto follower = makeFollowerManager();
  methods->releaseIndex(LogIndex{50});  // allow compaction upto 50

  EXPECT_CALL(*stateHandle, updateCommitIndex(LogIndex{55})).Times(1);
  EXPECT_CALL(*stateHandle, acquireSnapshot).Times(0);

  auto oldMessageId = ++lastMessageId;

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{40};  // compaction up to 40
    request.leaderCommit = LogIndex{55};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{2};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();
    EXPECT_TRUE(result.isSuccess());
  }

  {
    AppendEntriesRequest request;
    request.messageId = oldMessageId;
    request.lowestIndexToKeep = LogIndex{40};  // compaction up to 40
    request.leaderCommit = LogIndex{50};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{2};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    auto result = follower->appendEntries(request).get();
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.reason.error,
              AppendEntriesErrorReason::ErrorType::kMessageOutdated);
  }
}

TEST_F(AppendEntriesFollowerTest, resigned_follower) {
  termInfo->leader = "leader";
  termInfo->term = LogTerm{2};
  options->_thresholdLogCompaction = 0;

  auto methods = std::unique_ptr<IReplicatedLogFollowerMethods>{};
  EXPECT_CALL(*stateHandle, becomeFollower)
      .Times(1)
      .WillOnce([&](auto&& newMethods) { methods = std::move(newMethods); });
  EXPECT_CALL(*stateHandle, resignCurrentState).Times(1).WillOnce([&]() {
    return std::move(methods);
  });
  auto follower = makeFollowerManager();
  auto resigned = follower->resign();

  {
    AppendEntriesRequest request;
    request.messageId = ++lastMessageId;
    request.lowestIndexToKeep = LogIndex{40};  // compaction up to 40
    request.leaderCommit = LogIndex{55};
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{2};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{99}};
    EXPECT_THROW({ std::ignore = follower->appendEntries(request).get(); },
                 ParticipantResignedException);
  }
}
