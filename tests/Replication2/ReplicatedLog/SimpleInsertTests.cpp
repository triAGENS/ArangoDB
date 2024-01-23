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

#include <Basics/IndexIter.h>

#include "Replication2/ReplicatedLog/LogCore.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Metrics/Histogram.h"
#include "Metrics/LogScale.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::test;

TEST_F(ReplicatedLogTest, write_single_entry_to_follower) {
  auto coreA = makeLogCore(LogId{1});
  auto coreB = makeLogCore(LogId{2});

  auto leaderId = ParticipantId{"leader"};
  auto followerId = ParticipantId{"follower"};

  auto follower = std::make_shared<DelayedFollowerLog>(
      defaultLogger(), _logMetricsMock, _optionsMock, followerId,
      std::move(coreB), LogTerm{1}, leaderId);
  auto leader = createLeaderWithDefaultFlags(leaderId, LogTerm{1},
                                             std::move(coreA), {follower}, 2);

  auto countHistogramEntries = [](auto const& histogram) {
    auto [begin, end] =
        makeIndexIterPair([&](std::size_t i) { return histogram->load(i); }, 0,
                          histogram->size());

    return std::accumulate(
        begin, end, typename std::decay_t<decltype(*histogram)>::ValueType(0));
  };

  {
    // Nothing written on the leader except for term entry
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{1});
  }
  {
    // Nothing written on the follower
    auto status = std::get<FollowerStatus>(follower->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }
  {
    // Metric still unused
    auto numAppendEntries =
        countHistogramEntries(_logMetricsMock->replicatedLogAppendEntriesRttUs);
    EXPECT_EQ(numAppendEntries, 0U);
    auto numFollowerAppendEntries = countHistogramEntries(
        _logMetricsMock->replicatedLogFollowerAppendEntriesRtUs);
    EXPECT_EQ(numFollowerAppendEntries, 0U);
  }

  {
    // Insert first entry on the follower, expect the spearhead to be one
    auto idx = leader->insert(LogPayload::createFromString("first entry"),
                              false, LogLeader::doNotTriggerAsyncReplication);
    {
      auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
      EXPECT_EQ(status.local.commitIndex, LogIndex{0});
      EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
    }
    {
      auto status =
          std::get<FollowerStatus>(follower->getStatus().getVariant());
      EXPECT_EQ(status.local.commitIndex, LogIndex{0});
      EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
    }
    auto f = leader->waitFor(idx);
    EXPECT_FALSE(f.isReady());

    // Nothing sent to the follower yet, but only after triggerAsyncReplication
    EXPECT_FALSE(follower->hasPendingAppendEntries());
    leader->triggerAsyncReplication();
    EXPECT_TRUE(follower->hasPendingAppendEntries());

    {
      // check the leader log, there should be two entries written
      auto entry = std::optional<LogEntry>{};
      auto followerLog = getPersistedLogById(LogId{1});
      auto iter = followerLog->read(LogIndex{1});

      entry = iter->next();
      ASSERT_TRUE(entry.has_value());
      EXPECT_EQ(entry->logIndex(), LogIndex{1});
      EXPECT_FALSE(entry->hasPayload());

      entry = iter->next();
      EXPECT_TRUE(entry.has_value())
          << "expect one entry in leader log, found nothing";
      if (entry.has_value()) {
        EXPECT_EQ(entry->logIndex(), LogIndex{2});
        EXPECT_EQ(entry->logTerm(), LogTerm{1});
        EXPECT_EQ(*entry->logPayload(),
                  LogPayload::createFromString("first entry"));
      }

      entry = iter->next();
      EXPECT_FALSE(entry.has_value());
    }

    // Run async step, now the future should be fulfilled
    EXPECT_FALSE(f.isReady());
    follower->runAsyncAppendEntries();
    EXPECT_TRUE(f.isReady());

    {
      // Leader commit index is 2
      auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
      EXPECT_EQ(status.local.commitIndex, LogIndex{2});
      EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
    }
    {
      // Follower has spearhead 1 and commitIndex still 0
      auto status =
          std::get<FollowerStatus>(follower->getStatus().getVariant());
      EXPECT_EQ(status.local.commitIndex, LogIndex{0});
      EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
    }

    {
      // check the follower log, there should be two entries written
      auto entry = std::optional<LogEntry>{};
      auto followerLog = getPersistedLogById(LogId{2});
      auto iter = followerLog->read(LogIndex{1});

      entry = iter->next();
      ASSERT_TRUE(entry.has_value());
      EXPECT_EQ(entry->logIndex(), LogIndex{1});
      EXPECT_FALSE(entry->hasPayload());

      entry = iter->next();
      ASSERT_TRUE(entry.has_value())
          << "expect one entry in follower log, found nothing";
      EXPECT_EQ(entry->logIndex(), LogIndex{2});
      EXPECT_EQ(entry->logTerm(), LogTerm{1});
      EXPECT_EQ(*entry->logPayload(),
                LogPayload::createFromString("first entry"));

      entry = iter->next();
      EXPECT_FALSE(entry.has_value());
    }

    {
      // Expect the quorum to consist of the follower and the leader
      ASSERT_TRUE(f.isReady());
      auto result = f.get();
      EXPECT_EQ(result.currentCommitIndex, LogIndex{2});
      EXPECT_EQ(result.quorum->index, LogIndex{2});
      EXPECT_EQ(result.quorum->term, LogTerm{1});
      auto quorum = result.quorum->quorum;
      std::sort(quorum.begin(), quorum.end());
      EXPECT_EQ(quorum, (std::vector<ParticipantId>{followerId, leaderId}));
    }

    // Follower should have pending append entries
    // containing the commitIndex update
    EXPECT_TRUE(follower->hasPendingAppendEntries());
    follower->runAsyncAppendEntries();

    {
      // Follower has commitIndex 1
      auto status =
          std::get<FollowerStatus>(follower->getStatus().getVariant());
      EXPECT_EQ(status.local.commitIndex, LogIndex{2});
      EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
    }

    // LCI update
    EXPECT_TRUE(follower->hasPendingAppendEntries());
    follower->runAsyncAppendEntries();
    EXPECT_FALSE(follower->hasPendingAppendEntries());
  }
  {
    // Metric should have registered six appendEntries.
    // There was one insert, resulting in one appendEntries each to the follower
    // and the local follower. After the followers responded, the commit index
    // is updated, and both followers get another appendEntries request.
    // Finally, the LCI is updated with another round of requests.
    auto numAppendEntries =
        countHistogramEntries(_logMetricsMock->replicatedLogAppendEntriesRttUs);
    EXPECT_EQ(numAppendEntries, 6U);
    auto numFollowerAppendEntries = countHistogramEntries(
        _logMetricsMock->replicatedLogFollowerAppendEntriesRtUs);
    EXPECT_EQ(numFollowerAppendEntries, 6U);
  }
}

TEST_F(ReplicatedLogTest, wake_up_as_leader_with_persistent_data) {
  auto const entries = {
      replication2::LogEntry(LogTerm{1}, LogIndex{1},
                             LogPayload::createFromString("first entry")),
      replication2::LogEntry(LogTerm{1}, LogIndex{2},
                             LogPayload::createFromString("second entry")),
      replication2::LogEntry(LogTerm{2}, LogIndex{3},
                             LogPayload::createFromString("third entry"))};

  auto coreA = std::unique_ptr<LogCore>(nullptr);
  {
    auto leaderLog = makePersistedLog(LogId{1});
    for (auto const& entry : entries) {
      leaderLog->setEntry(entry);
    }
    coreA = std::make_unique<LogCore>(leaderLog);
  }

  auto leaderId = ParticipantId{"leader"};
  auto followerId = ParticipantId{"follower"};

  auto coreB = makeLogCore(LogId{2});
  auto follower = std::make_shared<DelayedFollowerLog>(
      defaultLogger(), _logMetricsMock, _optionsMock, followerId,
      std::move(coreB), LogTerm{3}, leaderId);
  auto leader = createLeaderWithDefaultFlags(leaderId, LogTerm{3},
                                             std::move(coreA), {follower}, 2);

  {
    // Leader should know it spearhead, but commitIndex is 0
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index,
              LogIndex{4});  // 3 + 1 from construct
  }
  {
    // Nothing written on the follower
    auto status = std::get<FollowerStatus>(follower->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }

  // Nothing should be ready
  auto f = leader->waitFor(LogIndex{3});
  EXPECT_FALSE(f.isReady());

  // this should trigger a sendAppendEntries to all follower
  EXPECT_FALSE(follower->hasPendingAppendEntries());
  leader->triggerAsyncReplication();
  EXPECT_TRUE(follower->hasPendingAppendEntries());
  {
    std::size_t number_of_runs = 0;
    while (follower->hasPendingAppendEntries()) {
      follower->runAsyncAppendEntries();
      number_of_runs += 1;
    }
    // AppendEntries with prevLogIndex 2 -> success = false
    // AppendEntries with prevLogIndex 0 -> success = true
    // AppendEntries with new commitIndex
    // AppendEntries with new LCI
    EXPECT_EQ(number_of_runs, 4U);
  }

  {
    // Leader has replicated all 4 entries
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{4});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{4});
  }
  {
    // Follower knows that everything is replicated
    auto status = std::get<FollowerStatus>(follower->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{4});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{4});
  }

  {
    // check that the follower has all log entries in its store
    auto iter = getPersistedLogById(LogId{2})->read(LogIndex{0});
    for (auto const& test : entries) {
      auto follower_entry = iter->next();
      ASSERT_TRUE(follower_entry.has_value());
      EXPECT_EQ(follower_entry.value(), test);
    }

    auto last = iter->next();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->logIndex(), LogIndex{4});
    EXPECT_EQ(last->logPayload(), nullptr);
  }
}

TEST_F(ReplicatedLogTest, multiple_follower) {
  auto coreA = makeLogCore(LogId{1});
  auto coreB = makeLogCore(LogId{2});
  auto coreC = makeLogCore(LogId{3});

  auto leaderId = ParticipantId{"leader"};
  auto followerId_1 = ParticipantId{"follower1"};
  auto followerId_2 = ParticipantId{"follower2"};

  auto follower_1 = std::make_shared<DelayedFollowerLog>(
      defaultLogger(), _logMetricsMock, _optionsMock, followerId_1,
      std::move(coreB), LogTerm{1}, leaderId);
  auto follower_2 = std::make_shared<DelayedFollowerLog>(
      defaultLogger(), _logMetricsMock, _optionsMock, followerId_2,
      std::move(coreC), LogTerm{1}, leaderId);
  // create leader with write concern 3
  auto leader = createLeaderWithDefaultFlags(
      leaderId, LogTerm{1}, std::move(coreA), {follower_1, follower_2}, 3);

  auto index = leader->insert(LogPayload::createFromString("first entry"),
                              false, LogLeader::doNotTriggerAsyncReplication);
  EXPECT_EQ(index,
            LogIndex{2});  // first entry is for term, second is used entry
  auto future = leader->waitFor(index);
  EXPECT_FALSE(future.isReady());

  {
    // Leader has spearhead at 1 but not committed
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has nothing
    auto status =
        std::get<FollowerStatus>(follower_1->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }
  {
    // Follower has nothing
    auto status =
        std::get<FollowerStatus>(follower_2->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }

  // sendAppendEntries
  leader->triggerAsyncReplication();
  EXPECT_TRUE(follower_1->hasPendingAppendEntries());
  EXPECT_TRUE(follower_2->hasPendingAppendEntries());

  // follower 1 answers AppendEntries request
  follower_1->runAsyncAppendEntries();
  // We do not expect any requests pending
  EXPECT_FALSE(follower_1->hasPendingAppendEntries());
  EXPECT_FALSE(future.isReady());
  {
    // Leader has spearhead at 1 but not committed
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has written 1 entry but not committed
    auto status =
        std::get<FollowerStatus>(follower_1->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has nothing
    auto status =
        std::get<FollowerStatus>(follower_2->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }

  // handle append entries on second follower
  follower_2->runAsyncAppendEntries();
  // now write concern 3 is reached, future is ready
  // and update of commitIndex on both follower
  {
    ASSERT_TRUE(future.isReady());
    auto result = future.get();
    EXPECT_EQ(result.currentCommitIndex, LogIndex{2});
    EXPECT_EQ(result.quorum->term, LogTerm{1});
    EXPECT_EQ(result.quorum->index, LogIndex{2});
    auto quorum = result.quorum->quorum;
    std::sort(quorum.begin(), quorum.end());
    EXPECT_EQ(quorum, (std::vector{followerId_1, followerId_2, leaderId}));
  }

  EXPECT_TRUE(follower_1->hasPendingAppendEntries());
  EXPECT_TRUE(follower_2->hasPendingAppendEntries());
  {
    // Leader has committed 1
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{2});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has written 1 entry but not committed
    auto status =
        std::get<FollowerStatus>(follower_1->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has written 1 entry but not committed
    auto status =
        std::get<FollowerStatus>(follower_2->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }

  // LCI updates
  follower_1->runAsyncAppendEntries();
  EXPECT_FALSE(follower_1->hasPendingAppendEntries());  // no lci update yet
  follower_2->runAsyncAppendEntries();
  EXPECT_TRUE(follower_2->hasPendingAppendEntries());

  follower_1->runAsyncAppendEntries();
  EXPECT_FALSE(follower_1->hasPendingAppendEntries());
  follower_2->runAsyncAppendEntries();
  EXPECT_FALSE(follower_2->hasPendingAppendEntries());

  {
    // Leader has committed 1
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{2});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has committed 1
    auto status =
        std::get<FollowerStatus>(follower_1->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{2});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
  {
    // Follower has committed 1
    auto status =
        std::get<FollowerStatus>(follower_2->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{2});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{2});
  }
}

TEST_F(ReplicatedLogTest,
       write_concern_one_immediate_leader_commit_on_startup) {
  auto const entries = {
      replication2::LogEntry(LogTerm{1}, LogIndex{1},
                             LogPayload::createFromString("first entry")),
      replication2::LogEntry(LogTerm{1}, LogIndex{2},
                             LogPayload::createFromString("second entry")),
      replication2::LogEntry(LogTerm{2}, LogIndex{3},
                             LogPayload::createFromString("third entry"))};

  auto coreA = std::unique_ptr<LogCore>(nullptr);
  {
    auto leaderLog = makePersistedLog(LogId{1});
    for (auto const& entry : entries) {
      leaderLog->setEntry(entry);
    }
    coreA = std::make_unique<LogCore>(leaderLog);
  }

  auto leaderId = ParticipantId{"leader"};
  auto followerId = ParticipantId{"follower"};

  auto coreB = makeLogCore(LogId{2});
  auto follower = std::make_shared<DelayedFollowerLog>(
      defaultLogger(), _logMetricsMock, _optionsMock, followerId,
      std::move(coreB), LogTerm{3}, leaderId);
  // set write concern to one
  auto leader = createLeaderWithDefaultFlags(leaderId, LogTerm{3},
                                             std::move(coreA), {follower}, 1);

  leader->triggerAsyncReplication();

  {
    // Leader should know it spearhead and its commit index is 4
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{4});
    EXPECT_EQ(status.local.spearHead.index,
              LogIndex{4});  // 3 + 1 from construct
  }
  {
    // Nothing written on the follower
    auto status = std::get<FollowerStatus>(follower->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{0});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{0});
  }

  // Older entries should be ready
  auto f = leader->waitFor(LogIndex{3});
  EXPECT_TRUE(f.isReady());

  EXPECT_TRUE(follower->hasPendingAppendEntries());
  {
    std::size_t number_of_runs = 0;
    while (follower->hasPendingAppendEntries()) {
      follower->runAsyncAppendEntries();
      number_of_runs += 1;
    }
    // AppendEntries with prevLogIndex 2 -> success = false, replicated log
    // empty AppendEntries with prevLogIndex 2 -> success = true, including
    // commit index AppendEntries with LCI
    EXPECT_EQ(number_of_runs, 3U);
  }

  {
    // Leader has replicated all 4 entries
    auto status = std::get<LeaderStatus>(leader->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{4});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{4});
  }
  {
    // Follower knows that everything is replicated
    auto status = std::get<FollowerStatus>(follower->getStatus().getVariant());
    EXPECT_EQ(status.local.commitIndex, LogIndex{4});
    EXPECT_EQ(status.local.spearHead.index, LogIndex{4});
  }

  {
    // check that the follower has all log entries in its store
    auto iter = getPersistedLogById(LogId{2})->read(LogIndex{0});
    for (auto const& test : entries) {
      auto follower_entry = iter->next();
      ASSERT_TRUE(follower_entry.has_value());
      EXPECT_EQ(follower_entry.value(), test);
    }

    auto last = iter->next();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->logIndex(), LogIndex{4});
    EXPECT_EQ(last->logPayload(), nullptr);
  }
}
