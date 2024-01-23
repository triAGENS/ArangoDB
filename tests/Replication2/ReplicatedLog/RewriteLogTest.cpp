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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "Replication2/ReplicatedLog/LogCore.h"

#include "Replication2/ReplicatedLog/LogEntry.h"
#include "Replication2/ReplicatedLog/LogStatus.h"

#include "Replication2/Helper/ReplicatedLogTestSetup.h"

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

struct RewriteLogTest : ReplicatedLogTest {};

TEST_F(RewriteLogTest, rewrite_old_leader) {
  // create one log that has three entries:
  // (1:1), (2:2), (3:2)
  auto followerLogContainer = createParticipant(
      {.initialLogRange = LogRange{LogIndex{1}, LogIndex{2}}});
  followerLogContainer->storageContext->emplaceLogRange(
      LogRange{LogIndex{2}, LogIndex{4}}, LogTerm{2});

  // create different log that has only one entry
  // (1:1)
  auto leaderLogContainer = createParticipant(
      {.initialLogRange = LogRange{LogIndex{1}, LogIndex{2}}});

  auto config = addNewTerm(leaderLogContainer->serverId(),
                           {followerLogContainer->serverId()},
                           {.term = LogTerm{3}, .writeConcern = 2});
  config->installConfig(false);

  auto leader = leaderLogContainer->log;
  auto follower = followerLogContainer->log;

  {
    auto stats = std::get<LeaderStatus>(leader->getStatus().getVariant()).local;
    EXPECT_EQ(stats.commitIndex, LogIndex{0});
    // Note that the leader inserts an empty log entry to establish leadership
    EXPECT_EQ(stats.spearHead, TermIndexPair(LogTerm(3), LogIndex(2)));
  }
  {
    auto stats =
        std::get<FollowerStatus>(follower->getStatus().getVariant()).local;
    EXPECT_EQ(stats.commitIndex, LogIndex{0});
    EXPECT_EQ(stats.spearHead, TermIndexPair(LogTerm(2), LogIndex(3)));
  }

  EXPECT_CALL(*followerLogContainer->stateHandleMock,
              updateCommitIndex(testing::Eq(LogIndex{2})));

  // have the leader send the append entries request;
  // have the follower process the append entries request.
  // this should rewrite its log.
  IHasScheduler::runAll(leaderLogContainer, followerLogContainer);

  EXPECT_FALSE(
      IHasScheduler::hasWork(leaderLogContainer, followerLogContainer));

  {
    auto stats = std::get<LeaderStatus>(leader->getStatus().getVariant()).local;
    EXPECT_EQ(stats.commitIndex, LogIndex{2});
    EXPECT_EQ(stats.spearHead, TermIndexPair(LogTerm(3), LogIndex(2)));
  }
  {
    auto stats =
        std::get<FollowerStatus>(follower->getStatus().getVariant()).local;
    EXPECT_EQ(stats.commitIndex, LogIndex{2});
    EXPECT_EQ(stats.spearHead, TermIndexPair(LogTerm(3), LogIndex(2)));
  }

  {
    auto entries = PartialLogEntries{
        {.term = 1_T, .index = 1_Lx, .payload = PartialLogEntry::IsPayload{}},
        {.term = 3_T, .index = 2_Lx, .payload = PartialLogEntry::IsMeta{}},
    };
    auto expectedEntries = testing::Pointwise(MatchesMapLogEntry(), entries);
    // check the follower's log: this must have been rewritten
    EXPECT_THAT(followerLogContainer->storageContext->log, expectedEntries);
    // just for completeness' sake, check the leader's log as well
    EXPECT_THAT(leaderLogContainer->storageContext->log, expectedEntries);
  }
}

#endif
