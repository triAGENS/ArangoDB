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

#include "Replication2/Helper/ReplicatedLogTestSetup.h"

#include "Basics/voc-errors.h"

// TODO Remove this conditional as soon as we've upgraded MSVC.
#ifdef DISABLE_I_HAS_SCHEDULER
#pragma message(                                                     \
    "Warning: Not compiling these tests due to a compiler bug, see " \
    "IHasScheduler.h for details.")
#else

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::test;

struct LeaderAppendEntriesTest : ReplicatedLogTest {};

TEST_F(LeaderAppendEntriesTest, simple_append_entries) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer = createFakeFollower();

  auto config = addNewTerm(leaderLogContainer->serverId(),
                           {followerLogContainer->serverId()},
                           {.term = 4_T, .writeConcern = 2});
  config->installConfig(true);

  auto const firstIdx =
      leaderLogContainer->insert(LogPayload::createFromString("first entry"));
  // Note that the leader inserts an empty log entry to establish leadership
  ASSERT_EQ(firstIdx, LogIndex{2});

  leaderLogContainer->runAll();

  auto follower = followerLogContainer->fakeAbstractFollower;
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();

    // Note that the leader inserted an empty log entry while establishing
    // leadership, and sent another message to update the commit index etc.
    EXPECT_EQ(req.messageId, MessageId{3});
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, 4_T);
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
  }

  follower->resolveWithOk();

  EXPECT_EQ(leaderLogContainer->getQuickStatus().local.commitIndex, firstIdx);

  leaderLogContainer->runAll();

  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{4});
    EXPECT_EQ(req.entries.size(), 0U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, firstIdx);
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, firstIdx);
  }
}

TEST_F(LeaderAppendEntriesTest, response_exception) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer = createFakeFollower();
  auto fakeFollower = followerLogContainer->fakeAbstractFollower;

  auto config = addNewTerm(leaderLogContainer->serverId(),
                           {followerLogContainer->serverId()},
                           {.term = 4_T, .writeConcern = 2});
  config->installConfig(true);

  // Note that the leader inserts an empty log entry when establishing
  // leadership
  EXPECT_EQ(leaderLogContainer->getQuickStatus().local.commitIndex,
            LogIndex{1});

  auto const firstIdx =
      leaderLogContainer->insert(LogPayload::createFromString("first entry"));
  ASSERT_EQ(firstIdx, LogIndex{2});

  leaderLogContainer->runAll();

  ASSERT_TRUE(fakeFollower->hasPendingRequests());
  {
    auto req = fakeFollower->currentRequest();
    // Note that the leader inserted an empty log entry while establishing
    // leadership, and sent another message to update the commit index etc.
    EXPECT_EQ(req.messageId, MessageId{3});
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
  }

  fakeFollower->resolveRequestWithException(std::logic_error("logic error"));

  ASSERT_FALSE(fakeFollower->hasPendingRequests());

  // We expect the leader to retry, but not commit anything
  leaderLogContainer->runAll();
  EXPECT_EQ(leaderLogContainer->getQuickStatus().local.commitIndex,
            LogIndex{1});
  ASSERT_TRUE(fakeFollower->hasPendingRequests());

  {
    auto req = fakeFollower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{4});
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
  }
}

TEST_F(LeaderAppendEntriesTest, test_wait_for_sync_flag_set_by_config) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer = createFakeFollower();
  auto fakeFollower = followerLogContainer->fakeAbstractFollower;

  // waitForSync=true in the config
  auto config = addNewTerm(
      leaderLogContainer->serverId(), {followerLogContainer->serverId()},
      {.term = 4_T, .writeConcern = 2, .waitForSync = true});
  config->installConfig(true);

  // waitForSync=false in the request
  auto const firstIdx = leaderLogContainer->insert(
      LogPayload::createFromString("first entry"), false);
  // Note that the leader inserts an empty log entry when establishing
  // leadership
  ASSERT_EQ(firstIdx, LogIndex{2});

  leaderLogContainer->runAll();
  ASSERT_TRUE(fakeFollower->hasPendingRequests());
  {
    auto req = fakeFollower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{3});
    // Note that the leader inserts an empty log entry in becomeLeader already
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
    EXPECT_TRUE(req.waitForSync);
  }
}

TEST_F(LeaderAppendEntriesTest, test_wait_for_sync_flag_set_by_param) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer = createFakeFollower();
  auto fakeFollower = followerLogContainer->fakeAbstractFollower;

  // waitForSync=false in the config
  auto config = addNewTerm(
      leaderLogContainer->serverId(), {followerLogContainer->serverId()},
      {.term = 4_T, .writeConcern = 2, .waitForSync = false});
  config->installConfig(true);

  // waitForSync=true in the request
  auto const firstIdx = leaderLogContainer->insert(
      LogPayload::createFromString("first entry"), true);
  // Note that the leader inserts an empty log entry while establishing
  // leadership
  ASSERT_EQ(firstIdx, LogIndex{2});

  leaderLogContainer->runAll();
  ASSERT_TRUE(fakeFollower->hasPendingRequests());
  {
    auto req = fakeFollower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{3});
    // Note that the leader inserts an empty log entry in becomeLeader already
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
    EXPECT_TRUE(req.waitForSync);
  }
}

TEST_F(LeaderAppendEntriesTest, test_wait_for_sync_flag_unset) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer = createFakeFollower();
  auto fakeFollower = followerLogContainer->fakeAbstractFollower;

  auto config = addNewTerm(leaderLogContainer->serverId(),
                           {followerLogContainer->serverId()},
                           {.term = 4_T, .writeConcern = 2});
  config->installConfig(false);

  // The first entry written by the leader has always set the waitForSync flag
  leaderLogContainer->runAll();
  ASSERT_TRUE(fakeFollower->hasPendingRequests());
  {
    auto req = fakeFollower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{1});
    // Note that the leader inserts an empty log entry to establish leadership
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
    EXPECT_TRUE(req.waitForSync);
  }

  IHasScheduler::runAll(leaderLogContainer, followerLogContainer);

  auto const firstIdx = leaderLogContainer->insert(
      LogPayload::createFromString("first entry"), false);
  ASSERT_EQ(firstIdx, LogIndex{2});

  leaderLogContainer->runAll();
  ASSERT_TRUE(fakeFollower->hasPendingRequests());
  {
    auto req = fakeFollower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{3});
    // Note that the leader inserts an empty log entry in becomeLeader already
    EXPECT_EQ(req.entries.size(), 1U);
    EXPECT_EQ(req.leaderId, leaderLogContainer->serverId());
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
    EXPECT_FALSE(req.waitForSync);
  }
}

TEST_F(LeaderAppendEntriesTest, propagate_largest_common_index) {
  auto leaderLogContainer = createParticipant({});
  auto followerLogContainer1 = createFakeFollower();
  auto followerLogContainer2 = createFakeFollower();
  auto fakeFollower1 = followerLogContainer1->fakeAbstractFollower;
  auto fakeFollower2 = followerLogContainer2->fakeAbstractFollower;

  auto config = addNewTerm(
      leaderLogContainer->serverId(),
      {followerLogContainer1->serverId(), followerLogContainer2->serverId()},
      {.term = 4_T, .writeConcern = 2});
  config->installConfig(false);

  /*
   * Three participants with writeConcern two. The commitIndex is updated as
   * soon as two servers have acknowledged the index, the litk is updated as
   * soon as the servers acknowledged the new sync index.
   */
  leaderLogContainer->runAll();
  // the leader has written one entry
  ASSERT_TRUE(fakeFollower1->hasPendingRequests());
  ASSERT_TRUE(fakeFollower2->hasPendingRequests());

  {
    auto stats = leaderLogContainer->getQuickStatus();
    EXPECT_EQ(stats.local.commitIndex, LogIndex{0});
    EXPECT_EQ(stats.local.lowestIndexToKeep, LogIndex{0});
  }

  {
    auto const& request = fakeFollower1->requests.front().request;
    EXPECT_EQ(request.leaderCommit, LogIndex{0});
    EXPECT_EQ(request.lowestIndexToKeep, LogIndex{0});
  }
  // let follower1 run and raise the commit index
  fakeFollower1->setSyncIndex(LogIndex{1});
  fakeFollower1->resolveWithOk();
  leaderLogContainer->runAll();

  {
    auto stats = leaderLogContainer->getQuickStatus();
    EXPECT_EQ(stats.local.commitIndex, LogIndex{1});
    EXPECT_EQ(stats.local.lowestIndexToKeep, LogIndex{0});
  }

  // follower1 received a message with that commitIndex is now 1
  ASSERT_TRUE(fakeFollower1->hasPendingRequests());
  {
    auto const& request = fakeFollower1->requests.front().request;
    EXPECT_EQ(request.leaderCommit, LogIndex{1});
    EXPECT_EQ(request.lowestIndexToKeep, LogIndex{0});
  }
  fakeFollower1->resolveWithOk();
  leaderLogContainer->runAll();
  // there are now no more requests pending for follower1
  ASSERT_FALSE(fakeFollower1->hasPendingRequests());

  // let follower2 run - this does not raise the litk because follower2 still
  // responds with syncIndex=0
  fakeFollower2->resolveWithOk();
  leaderLogContainer->runAll();

  {
    auto stats = leaderLogContainer->getQuickStatus();
    EXPECT_EQ(stats.local.commitIndex, LogIndex{1});
    EXPECT_EQ(stats.local.lowestIndexToKeep, LogIndex{0});
  }

  // follower2 is now updated with the commit index
  ASSERT_TRUE(fakeFollower2->hasPendingRequests());
  {
    auto const& request = fakeFollower2->requests.front().request;
    EXPECT_EQ(request.leaderCommit, LogIndex{1});
    EXPECT_EQ(request.lowestIndexToKeep, LogIndex{0});
  }
  // now "sync" the log entry and respond on follower2
  fakeFollower2->setSyncIndex(LogIndex{1});
  fakeFollower2->resolveWithOk();
  leaderLogContainer->runAll();

  {
    auto stats = leaderLogContainer->getQuickStatus();
    EXPECT_EQ(stats.local.commitIndex, LogIndex{1});
    EXPECT_EQ(stats.local.lowestIndexToKeep, LogIndex{1});
  }

  // now follower2 received an update with litk = 1
  ASSERT_TRUE(fakeFollower2->hasPendingRequests());
  {
    auto const& request = fakeFollower2->requests.front().request;
    EXPECT_EQ(request.leaderCommit, LogIndex{1});
    EXPECT_EQ(request.lowestIndexToKeep, LogIndex{1});
  }

  // now follower1 received an update with litk = 1
  ASSERT_TRUE(fakeFollower1->hasPendingRequests());
  {
    auto const& request = fakeFollower1->requests.front().request;
    EXPECT_EQ(request.leaderCommit, LogIndex{1});
    EXPECT_EQ(request.lowestIndexToKeep, LogIndex{1});
  }
}

#endif
