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

#include "Basics/voc-errors.h"

#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedLog/types.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

struct LeaderAppendEntriesTest : ReplicatedLogTest {};

struct FakeFollower : AbstractFollower {
  FakeFollower(ParticipantId id) : participantId(std::move(id)) {}

  [[nodiscard]] auto getParticipantId() const noexcept -> ParticipantId const& override {
    return participantId;
  };

  virtual auto appendEntries(AppendEntriesRequest request)
      -> futures::Future<AppendEntriesResult> override {
    return requests.emplace_back(std::move(request)).promise.getFuture();
  }

  void resolveRequest(AppendEntriesResult result) {
    requests.front().promise.setValue(std::move(result));
    requests.pop_front();
  }

  template<typename E>
  void resolveRequestWithException(E&& e) {
    requests.front().promise.setException(std::forward<E>(e));
    requests.pop_front();
  }

  auto currentRequest() const -> AppendEntriesRequest const& {
    return requests.front().request;
  }

  auto hasPendingRequests() const -> bool {
    return !requests.empty();
  }

  struct AsyncRequest {
    explicit AsyncRequest(AppendEntriesRequest request)
        : request(std::move(request)) {}
    AppendEntriesRequest request;
    futures::Promise<AppendEntriesResult> promise;
  };

  std::deque<AsyncRequest> requests;
  ParticipantId participantId;
};

TEST_F(LeaderAppendEntriesTest, simple_append_entries) {
  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeFollower>("follower");
  auto leader = leaderLog->becomeLeader("leader", LogTerm{4}, {follower}, 2);

  {
    auto idx = leader->insert(LogPayload{"first entry"});
    ASSERT_EQ(idx, LogIndex{1});
  }

  leader->runAsyncStep();
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{1});
    EXPECT_EQ(req.entries.size(), 1);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
  }

  {
    auto result = AppendEntriesResult{LogTerm{4}, TRI_ERROR_NO_ERROR,
                                      AppendEntriesErrorReason::NONE,
                                      follower->currentRequest().messageId};
    follower->resolveRequest(std::move(result));
  }

  {
    auto stats = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(stats.local.commitIndex, LogIndex{1});
  }

  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{2});
    EXPECT_EQ(req.entries.size(), 0);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{4});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{1});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{1});
  }
}

TEST_F(LeaderAppendEntriesTest, response_exception) {
  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeFollower>("follower");
  auto leader = leaderLog->becomeLeader("leader", LogTerm{4}, {follower}, 2);

  {
    auto idx = leader->insert(LogPayload{"first entry"});
    ASSERT_EQ(idx, LogIndex{1});
  }

  leader->runAsyncStep();
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{1});
    EXPECT_EQ(req.entries.size(), 1);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
  }

  {
    follower->resolveRequestWithException(std::logic_error("logic error"));
  }

  {
    auto stats = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(stats.local.commitIndex, LogIndex{0}); // do not commit yet
  }

  // we expect a retry
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{2});
    EXPECT_EQ(req.entries.size(), 1);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
  }
}

TEST_F(LeaderAppendEntriesTest, test_wait_for_sync_flag_set) {
  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeFollower>("follower");

  auto config = LogConfig{};
  config.waitForSync = true;
  config.writeConcern = 2;
  auto leader = leaderLog->becomeLeader(config, "leader", LogTerm{4}, {follower});

  {
    auto idx = leader->insert(LogPayload{"first entry"});
    ASSERT_EQ(idx, LogIndex{1});
  }

  leader->runAsyncStep();
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{1});
    EXPECT_EQ(req.entries.size(), 1);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
    EXPECT_TRUE(req.waitForSync);
  }
}

TEST_F(LeaderAppendEntriesTest, test_wait_for_sync_flag_unset) {
  auto leaderLog = makeReplicatedLog(LogId{1});
  auto follower = std::make_shared<FakeFollower>("follower");
  auto leader = leaderLog->becomeLeader("leader", LogTerm{4}, {follower}, 2);

  {
    auto idx = leader->insert(LogPayload{"first entry"});
    ASSERT_EQ(idx, LogIndex{1});
  }

  leader->runAsyncStep();
  ASSERT_TRUE(follower->hasPendingRequests());
  {
    auto req = follower->currentRequest();
    EXPECT_EQ(req.messageId, MessageId{1});
    EXPECT_EQ(req.entries.size(), 1);
    EXPECT_EQ(req.leaderId, ParticipantId {"leader"});
    EXPECT_EQ(req.prevLogEntry.term, LogTerm{0});
    EXPECT_EQ(req.prevLogEntry.index, LogIndex{0});
    EXPECT_EQ(req.leaderTerm, LogTerm{4});
    EXPECT_EQ(req.leaderCommit, LogIndex{0});
    EXPECT_FALSE(req.waitForSync);
  }
}
