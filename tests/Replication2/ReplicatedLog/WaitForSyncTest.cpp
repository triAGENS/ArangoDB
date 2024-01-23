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

#include "Replication2/Helper/TestHelper.h"

#include "Basics/voc-errors.h"

#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"

#include "Replication2/Mocks/FakeAbstractFollower.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::test;

struct WaitForSyncTest : ReplicatedLogTest {};

TEST_F(WaitForSyncTest, no_wait_for_sync) {
  auto const waitForSync = false;
  auto const term = LogTerm{4};

  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeAbstractFollower>("follower");
  auto leader =
      leaderLog->becomeLeader("leader", term, {follower}, 2, waitForSync);

  // first entry is always with waitForSync
  leader->triggerAsyncReplication();
  follower->handleAllRequestsWithOk();

  auto const firstIdx =
      leader->insert(LogPayload::createFromString("first entry"));
  // Note that the leader inserts an empty log entry in becomeLeader already
  ASSERT_EQ(firstIdx, LogIndex{2});

  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{4});
    EXPECT_FALSE(req.waitForSync);
  }

  {
    auto result = AppendEntriesResult{
        term, TRI_ERROR_NO_ERROR, {}, follower->currentRequest().messageId};
    follower->resolveRequest(std::move(result));
  }
}

TEST_F(WaitForSyncTest, global_wait_for_sync) {
  auto const waitForSync = true;
  auto const term = LogTerm{4};

  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeAbstractFollower>("follower");
  auto leader =
      leaderLog->becomeLeader("leader", term, {follower}, 2, waitForSync);

  // first entry is always with waitForSync
  leader->triggerAsyncReplication();
  follower->handleAllRequestsWithOk();

  auto const firstIdx =
      leader->insert(LogPayload::createFromString("first entry"));
  // Note that the leader inserts an empty log entry in becomeLeader already
  ASSERT_EQ(firstIdx, LogIndex{2});

  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{4});
    EXPECT_TRUE(req.waitForSync);
  }

  {
    auto result = AppendEntriesResult{
        term, TRI_ERROR_NO_ERROR, {}, follower->currentRequest().messageId};
    follower->resolveRequest(std::move(result));
  }
}

TEST_F(WaitForSyncTest, per_entry_wait_for_sync) {
  auto const waitForSync = false;
  auto const term = LogTerm{4};

  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeAbstractFollower>("follower");
  auto leader =
      leaderLog->becomeLeader("leader", term, {follower}, 2, waitForSync);

  // first entry is always with waitForSync
  leader->triggerAsyncReplication();
  follower->handleAllRequestsWithOk();

  auto const firstIdx =
      leader->insert(LogPayload::createFromString("first entry"), true);
  // Note that the leader inserts an empty log entry in becomeLeader already
  ASSERT_EQ(firstIdx, LogIndex{2});

  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{4});
    EXPECT_TRUE(req.waitForSync);
  }

  {
    auto result = AppendEntriesResult{
        term, TRI_ERROR_NO_ERROR, {}, follower->currentRequest().messageId};
    follower->resolveRequest(std::move(result));
  }
}

struct WaitForSyncPersistorTest : ReplicatedLogTest {};

TEST_F(WaitForSyncPersistorTest, wait_for_sync_entry) {
  auto leaderId = LogId{1};
  auto followerId = LogId{2};
  bool const waitForSyncGlobal = false;

  auto followerLog = makeReplicatedLog(followerId);
  auto follower = followerLog->becomeFollower("follower", LogTerm{1}, "leader");

  auto leaderLog = makeReplicatedLog(leaderId);
  auto leader = leaderLog->becomeLeader("leader", LogTerm{1}, {follower}, 2,
                                        waitForSyncGlobal);
  leader->triggerAsyncReplication();

  auto leaderPersisted = _persistedLogs.at(leaderId);
  auto followerPersisted = _persistedLogs.at(followerId);

  auto idx = leader->insert(LogPayload::createFromString("first entry"));
  follower->runAllAsyncAppendEntries();
  EXPECT_FALSE(leaderPersisted->checkEntryWaitedForSync(idx));
  EXPECT_FALSE(followerPersisted->checkEntryWaitedForSync(idx));
  auto idx2 =
      leader->insert(LogPayload::createFromString("second entry"), true);
  follower->runAllAsyncAppendEntries();
  EXPECT_TRUE(leaderPersisted->checkEntryWaitedForSync(idx2));
  EXPECT_TRUE(followerPersisted->checkEntryWaitedForSync(idx2));
}

TEST_F(WaitForSyncPersistorTest, wait_for_sync_global) {
  auto leaderId = LogId{1};
  auto followerId = LogId{2};
  bool const waitForSyncGlobal = true;

  auto followerLog = makeReplicatedLog(followerId);
  auto follower = followerLog->becomeFollower("follower", LogTerm{1}, "leader");

  auto leaderLog = makeReplicatedLog(leaderId);
  auto leader = leaderLog->becomeLeader("leader", LogTerm{1}, {follower}, 2,
                                        waitForSyncGlobal);
  leader->triggerAsyncReplication();

  auto leaderPersisted = _persistedLogs.at(leaderId);
  auto followerPersisted = _persistedLogs.at(followerId);

  auto idx = leader->insert(LogPayload::createFromString("first entry"));
  follower->runAllAsyncAppendEntries();
  EXPECT_TRUE(leaderPersisted->checkEntryWaitedForSync(idx));
  EXPECT_TRUE(followerPersisted->checkEntryWaitedForSync(idx));
  auto idx2 =
      leader->insert(LogPayload::createFromString("second entry"), true);
  follower->runAllAsyncAppendEntries();
  EXPECT_TRUE(leaderPersisted->checkEntryWaitedForSync(idx2));
  EXPECT_TRUE(followerPersisted->checkEntryWaitedForSync(idx2));
}
