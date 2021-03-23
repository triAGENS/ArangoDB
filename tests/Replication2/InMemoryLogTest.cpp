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

#include <Replication2/InMemoryLog.h>

#include <gtest/gtest.h>

using namespace arangodb;
using namespace arangodb::replication2;

TEST(InMemoryLogTest, test) {
  auto const state = std::make_shared<InMemoryState>();
  auto const ourParticipantId = ParticipantId{1};
  auto log = InMemoryLog{ourParticipantId, state};

  log.becomeLeader(LogTerm{1}, {}, 1);

  {
    auto stats = log.getStatistics();
    ASSERT_EQ(LogIndex{0}, stats.commitIndex);
    ASSERT_EQ(LogIndex{0}, stats.spearHead);
  }

  auto index = log.insert(LogPayload{"myLogEntry 1"});
  ASSERT_EQ(LogIndex{1}, index);

  auto f = log.waitFor(index);

  {
    auto stats = log.getStatistics();
    ASSERT_EQ(LogIndex{0}, stats.commitIndex);
    ASSERT_EQ(LogIndex{1}, stats.spearHead);
  }

  log.runAsyncStep();

  ASSERT_TRUE(f.isReady());

  {
    auto stats = log.getStatistics();
    ASSERT_EQ(LogIndex{1}, stats.commitIndex);
    ASSERT_EQ(LogIndex{1}, stats.spearHead);
  }
}
