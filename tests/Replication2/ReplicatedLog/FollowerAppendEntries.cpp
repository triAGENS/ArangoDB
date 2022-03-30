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

#include "TestHelper.h"

#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"

#include "Replication2/Mocks/ReplicatedLogMetricsMock.h"

#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedLog/types.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::test;

struct FollowerAppendEntriesTest : ReplicatedLogTest {
  auto makeFollower(ParticipantId id, LogTerm term, ParticipantId leaderId)
      -> std::shared_ptr<ReplicatedLog> {
    auto core = makeLogCore(LogId{3});
    auto log = std::make_shared<ReplicatedLog>(std::move(core), _logMetricsMock,
                                               _optionsMock, defaultLogger());
    log->becomeFollower(std::move(id), term, std::move(leaderId));
    return log;
  }

  auto makeDelayedPersistedLog(LogId id) -> std::shared_ptr<DelayedMockLog> {
    auto persisted = std::make_shared<DelayedMockLog>(id);
    _persistedLogs[id] = persisted;
    return persisted;
  }

  auto makeDelayedFollower(ParticipantId id, LogTerm term,
                           ParticipantId leaderId)
      -> std::shared_ptr<ReplicatedLog> {
    auto persisted = makeDelayedPersistedLog(LogId{3});
    auto core = std::make_unique<LogCore>(persisted);
    auto log = std::make_shared<ReplicatedLog>(std::move(core), _logMetricsMock,
                                               _optionsMock, defaultLogger());
    log->becomeFollower(std::move(id), term, std::move(leaderId));
    return log;
  }

  MessageId nextMessageId{0};
};

TEST_F(FollowerAppendEntriesTest, valid_append_entries) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
      EXPECT_EQ(result.reason, AppendEntriesErrorReason{});
    }
  }

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request.leaderCommit = LogIndex{1};
    request.messageId = ++nextMessageId;
    request.entries = {};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
      EXPECT_EQ(result.reason, AppendEntriesErrorReason{});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, wrong_term) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{4};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kWrongTerm});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, missing_prev_log_index) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, missmatch_prev_log_term) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    // First add a valid entry
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      ASSERT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
    }
  }

  {
    // Now add another with wrong term in prevLogTerm
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{3}, LogIndex{1}};
    request.leaderCommit = LogIndex{1};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{5}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, wrong_leader_name) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    AppendEntriesRequest request;
    request.leaderId = "odlLeader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      // TODO this is known to fail
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kInvalidLeaderId});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, resigned_follower) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    // First add a valid entry
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      ASSERT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
    }
  }

  // drop() resigns the follower, and removes it as the participant from log.
  auto logCore = log->drop();
  // we should have gotten the actual logcore, and do now destroy it
  ASSERT_NE(logCore, nullptr);
  logCore.reset();
  // follower should now be resigned
  ASSERT_THROW(
      std::ignore = follower->getStatus(),
      ParticipantResignedException);  // TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{5}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kLostLogCore});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, outdated_message_id) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    // First add a valid entry
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = MessageId{5};
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      ASSERT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
    }
  }

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request.leaderCommit = LogIndex{0};
    request.messageId = MessageId{4};
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{5}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(result.reason,
                AppendEntriesErrorReason{
                    AppendEntriesErrorReason::ErrorType::kMessageOutdated});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, rewrite_log) {
  auto log = makeFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();

  {
    AppendEntriesRequest request;
    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{5}, LogIndex{20},
                           LogPayload::createFromString("some payload")))};

    auto f = follower->appendEntries(std::move(request));
    ASSERT_TRUE(f.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
      EXPECT_EQ(result.reason, AppendEntriesErrorReason{});
    }
  }

  {
    auto status = follower->getStatus();
    ASSERT_TRUE(std::holds_alternative<FollowerStatus>(status.getVariant()));
    auto fstatus = std::get<FollowerStatus>(status.getVariant());
    EXPECT_EQ(fstatus.local.firstIndex, LogIndex{20});
  }

  auto iter = follower->getLogIterator(LogIndex{1});
  {
    auto entry = iter->next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->logIndex(), LogIndex{20});
  }
  {
    auto entry = iter->next();
    ASSERT_FALSE(entry.has_value());
  }
}

TEST_F(FollowerAppendEntriesTest, deuplicate_append_entries_test) {
  auto log = makeDelayedFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();
  auto persisted =
      std::dynamic_pointer_cast<DelayedMockLog>(_persistedLogs[LogId{3}]);

  // send an append entry requires, this will not resolve immediately
  // due to the delayed mock log.
  AppendEntriesRequest request;
  request.leaderId = "leader";
  request.leaderTerm = LogTerm{5};
  request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
  request.leaderCommit = LogIndex{0};
  request.messageId = ++nextMessageId;
  request.entries = {InMemoryLogEntry(PersistingLogEntry(
      LogTerm{1}, LogIndex{1}, LogPayload::createFromString("some payload")))};

  auto f = follower->appendEntries(std::move(request));
  ASSERT_FALSE(f.isReady());
  ASSERT_TRUE(persisted->hasPendingInsert());

  // send another append entries request
  {
    AppendEntriesRequest request2;
    request2.leaderId = "leader";
    request2.leaderTerm = LogTerm{5};
    request2.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
    request2.leaderCommit = LogIndex{0};
    request2.messageId = ++nextMessageId;
    request2.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{1},
                           LogPayload::createFromString("some payload")))};

    auto f2 = follower->appendEntries(std::move(request2));
    ASSERT_TRUE(f2.isReady());
    {
      auto result = f2.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode,
                TRI_ERROR_REPLICATION_REPLICATED_LOG_APPEND_ENTRIES_REJECTED);
      EXPECT_EQ(
          result.reason,
          AppendEntriesErrorReason{
              AppendEntriesErrorReason::ErrorType::kPrevAppendEntriesInFlight});
    }
  }

  // now resolve the insertAsync
  persisted->runAsyncInsert();
  ASSERT_TRUE(f.isReady());
  {
    auto result = f.get();
    EXPECT_EQ(result.logTerm, LogTerm{5});
    EXPECT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
    EXPECT_EQ(result.reason, AppendEntriesErrorReason{});
  }

  // now try again
  // send another append entries request
  {
    AppendEntriesRequest request2;
    request2.leaderId = "leader";
    request2.leaderTerm = LogTerm{5};
    request2.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request2.leaderCommit = LogIndex{0};
    request2.messageId = ++nextMessageId;
    request2.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};

    auto f2 = follower->appendEntries(std::move(request2));
    persisted->runAsyncInsert();  // and resolve immediately
    ASSERT_TRUE(f2.isReady());
    {
      auto result = f.get();
      EXPECT_EQ(result.logTerm, LogTerm{5});
      EXPECT_EQ(result.errorCode, TRI_ERROR_NO_ERROR);
      EXPECT_EQ(result.reason, AppendEntriesErrorReason{});
    }
  }
}

TEST_F(FollowerAppendEntriesTest, append_entries_locking_regression_test) {
  auto log = makeDelayedFollower("follower", LogTerm{5}, "leader");
  auto follower = log->getFollower();
  auto persisted =
      std::dynamic_pointer_cast<DelayedMockLog>(_persistedLogs[LogId{3}]);

  // send an append entry requires, this will not resolve immediately
  // due to the delayed mock log.
  AppendEntriesRequest request;
  request.leaderId = "leader";
  request.leaderTerm = LogTerm{5};
  request.prevLogEntry = TermIndexPair{LogTerm{0}, LogIndex{0}};
  request.leaderCommit = LogIndex{0};
  request.messageId = ++nextMessageId;
  request.entries = {InMemoryLogEntry(PersistingLogEntry(
      LogTerm{1}, LogIndex{1}, LogPayload::createFromString("some payload")))};

  auto f = follower->appendEntries(request);
  ASSERT_FALSE(f.isReady());
  ASSERT_TRUE(persisted->hasPendingInsert());

  std::move(f).thenValue([&](auto&& res) {
    EXPECT_TRUE(res.errorCode == TRI_ERROR_NO_ERROR) << res.errorCode;
    ASSERT_FALSE(persisted->hasPendingInsert());

    request.leaderId = "leader";
    request.leaderTerm = LogTerm{5};
    request.prevLogEntry = TermIndexPair{LogTerm{1}, LogIndex{1}};
    request.leaderCommit = LogIndex{0};
    request.messageId = ++nextMessageId;
    request.entries = {InMemoryLogEntry(
        PersistingLogEntry(LogTerm{1}, LogIndex{2},
                           LogPayload::createFromString("some payload")))};
    // send another request to the follower. This will acquire the lock
    // again and should wait for the persistor to act.
    auto f2 = follower->appendEntries(request);
    EXPECT_FALSE(f2.isReady());

    // This will block without the fix.
    auto status = follower->getStatus();
  });

  persisted->runAsyncInsert();
  ASSERT_TRUE(persisted->hasPendingInsert());
  persisted->runAsyncInsert();
}
