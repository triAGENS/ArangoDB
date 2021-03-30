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

#ifndef ARANGOD_REPLICATION2_INMEMORYLOG_H
#define ARANGOD_REPLICATION2_INMEMORYLOG_H

#include <Futures/Future.h>
#include <velocypack/SharedSlice.h>
#include <immer/map.hpp>

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Replication2/PersistedLog.h"

#include "Common.h"

namespace arangodb::replication2 {

struct AppendEntriesResult {
  bool success = false;
  LogTerm logTerm = LogTerm{};
};

struct AppendEntriesRequest {
  LogTerm leaderTerm;
  ParticipantId leaderId;
  LogTerm prevLogTerm;
  LogIndex prevLogIndex;
  LogIndex leaderCommit;
  std::vector<LogEntry> entries;
};

/**
 * @brief State stub, later to be replaced by a RocksDB state.
 */

class InMemoryState {
 public:
  using state_container = immer::map<std::string, arangodb::velocypack::SharedSlice>;

  auto createSnapshot() -> std::shared_ptr<InMemoryState const>;

  // TODO We probably want an immer::map to be able to get snapshots
  state_container _state;

  explicit InMemoryState(state_container state);
};


struct LogFollower {
  virtual ~LogFollower() = default;

  [[nodiscard]] virtual auto participantId() const noexcept -> ParticipantId = 0;
  virtual auto appendEntries(AppendEntriesRequest)
      -> arangodb::futures::Future<AppendEntriesResult> = 0;
};

/**
 * @brief A simple non-persistent log implementation, mainly for prototyping
 * replication 2.0.
 */
class InMemoryLog : public LogFollower {
 public:
  InMemoryLog() = delete;
  explicit InMemoryLog(ParticipantId participantId, std::shared_ptr<InMemoryState> state,
                       std::shared_ptr<PersistedLog> persistedLog);

  // follower only
  auto appendEntries(AppendEntriesRequest) -> arangodb::futures::Future<AppendEntriesResult> override;

  // leader only
  auto insert(LogPayload) -> LogIndex;

  // leader only
  auto createSnapshot() -> std::pair<LogIndex, std::shared_ptr<InMemoryState const>>;

  // leader only
  auto waitFor(LogIndex) -> arangodb::futures::Future<arangodb::futures::Unit>;

  // Set to follower, and (strictly increase) term to the given value
  auto becomeFollower(LogTerm, ParticipantId leaderId) -> void;

  // Set to leader, and (strictly increase) term to the given value
  auto becomeLeader(LogTerm term, std::vector<std::shared_ptr<LogFollower>> const& follower,
                    std::size_t writeConcern) -> void;

  [[nodiscard]] auto getStatistics() const -> LogStatistics;

  auto runAsyncStep() -> void;

  [[nodiscard]] auto participantId() const noexcept -> ParticipantId override;

 protected:
  LogIndex nextIndex();
  void assertLeader() const;
  void assertFollower() const;

  [[nodiscard]] auto getEntryByIndex(LogIndex) const -> std::optional<LogEntry>;

 private:
  struct Follower {
    std::shared_ptr<LogFollower> _impl;
    LogIndex lastAckedIndex;
  };

  struct Unconfigured {};
  struct LeaderConfig {
    std::vector<Follower> follower{};
    std::size_t writeConcern{};
  };
  struct FollowerConfig {
    ParticipantId leaderId{};
  };

  std::variant<Unconfigured, LeaderConfig, FollowerConfig> _role;
  ParticipantId _id{};
  std::shared_ptr<PersistedLog> _persistedLog;
  LogIndex _persistedLogEnd{};
  LogTerm _currentTerm = LogTerm{};
  std::deque<LogEntry> _log;
  std::shared_ptr<InMemoryState> _state;
  LogIndex _commitIndex{};

  using WaitForPromise = futures::Promise<arangodb::futures::Unit>;
  std::multimap<LogIndex, WaitForPromise> _waitForQueue;
  void persistRemainingLogEntries();
};

}  // namespace arangodb::replication2

#endif  // ARANGOD_REPLICATION2_INMEMORYLOG_H
