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

#include <Basics/Guarded.h>
#include <Futures/Future.h>
#include <velocypack/SharedSlice.h>
#include <velocypack/Builder.h>
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

  void toVelocyPack(velocypack::Builder& builder) const;
  static auto fromVelocyPack(velocypack::Slice slice) -> AppendEntriesResult;
};

struct AppendEntriesRequest {
  LogTerm leaderTerm;
  ParticipantId leaderId;
  // TODO assert index == 0 <=> term == 0
  LogTerm prevLogTerm;
  LogIndex prevLogIndex;
  LogIndex leaderCommit;
  std::vector<LogEntry> entries;

  void toVelocyPack(velocypack::Builder& builder) const;
  static auto fromVelocyPack(velocypack::Slice slice) -> AppendEntriesRequest;
};

/**
 * @brief State stub, later to be replaced by a RocksDB state.
 */

class InMemoryState {
 public:
  using state_container = immer::map<std::string, arangodb::velocypack::SharedSlice>;

  auto createSnapshot() -> std::shared_ptr<InMemoryState const>;

  state_container _state;

  explicit InMemoryState(state_container state);
  InMemoryState() = default;
};


struct LogFollower {
  virtual ~LogFollower() = default;

  [[nodiscard]] virtual auto participantId() const noexcept -> ParticipantId = 0;
  virtual auto appendEntries(AppendEntriesRequest)
      -> arangodb::futures::Future<AppendEntriesResult> = 0;
};

struct QuorumData {
  QuorumData(const LogIndex& index, LogTerm term, std::vector<ParticipantId> quorum);

  LogIndex index;
  LogTerm term;
  std::vector<ParticipantId> quorum;
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
  auto waitFor(LogIndex) -> arangodb::futures::Future<std::shared_ptr<QuorumData>>;

  // Set to follower, and (strictly increase) term to the given value
  auto becomeFollower(LogTerm, ParticipantId leaderId) -> void;

  // Set to leader, and (strictly increase) term to the given value
  auto becomeLeader(LogTerm term, std::vector<std::shared_ptr<LogFollower>> const& follower,
                    std::size_t writeConcern) -> void;

  [[nodiscard]] auto getStatistics() const -> LogStatistics;

  auto runAsyncStep() -> void;

  [[nodiscard]] auto participantId() const noexcept -> ParticipantId override;

  [[nodiscard]] auto getEntryByIndex(LogIndex) const -> std::optional<LogEntry>;

 private:
  struct GuardedInMemoryLog;
  using Guard = MutexGuard<GuardedInMemoryLog, std::unique_lock<std::mutex>>;
  using ConstGuard = MutexGuard<GuardedInMemoryLog const, std::unique_lock<std::mutex>>;

  struct Follower {
    explicit Follower(std::shared_ptr<LogFollower> impl)
        : _impl(std::move(impl)) {}

    std::shared_ptr<LogFollower> _impl;
    LogIndex lastAckedIndex = LogIndex{0};
    LogIndex lastAckedCommitIndex = LogIndex{0};
    bool requestInFlight = false;
  };

  struct Unconfigured {};
  struct LeaderConfig {
    std::vector<Follower> follower{};
    std::size_t writeConcern{};
  };
  struct FollowerConfig {
    ParticipantId leaderId{};
  };

  using WaitForPromise = futures::Promise<std::shared_ptr<QuorumData>>;

  struct GuardedInMemoryLog {
    GuardedInMemoryLog() = delete;
    GuardedInMemoryLog(GuardedInMemoryLog const&) = delete;
    GuardedInMemoryLog(GuardedInMemoryLog&&) = delete;
    GuardedInMemoryLog& operator=(GuardedInMemoryLog const&) = delete;
    GuardedInMemoryLog& operator=(GuardedInMemoryLog&&) = delete;
    ~GuardedInMemoryLog() = default;
    GuardedInMemoryLog(ParticipantId id, std::shared_ptr<InMemoryState> state,
                   std::shared_ptr<PersistedLog> persistedLog, LogIndex logIndex)
        : _id(id), _persistedLog(std::move(persistedLog)), _state(std::move(state)), _commitIndex{logIndex} {}

    // follower only
    auto appendEntries(AppendEntriesRequest) -> arangodb::futures::Future<AppendEntriesResult>;

    // leader only
    auto insert(LogPayload) -> LogIndex;

    // leader only
    auto createSnapshot() -> std::pair<LogIndex, std::shared_ptr<InMemoryState const>>;

    // leader only
    auto waitFor(LogIndex) -> arangodb::futures::Future<std::shared_ptr<QuorumData>>;

    // Set to follower, and (strictly increase) term to the given value
    auto becomeFollower(LogTerm, ParticipantId leaderId) -> void;

    // Set to leader, and (strictly increase) term to the given value
    auto becomeLeader(LogTerm term, std::vector<std::shared_ptr<LogFollower>> const& follower,
                      std::size_t writeConcern) -> void;

    [[nodiscard]] auto getStatistics() const -> LogStatistics;

    auto runAsyncStep() -> void;

    [[nodiscard]] auto participantId() const noexcept -> ParticipantId;
    [[nodiscard]] auto getEntryByIndex(LogIndex) const -> std::optional<LogEntry>;

    LogIndex nextIndex() const;
    LogIndex getLastIndex() const;
    void assertLeader() const;
    void assertFollower() const;

    void checkCommitIndex();
    void updateCommitIndexLeader(LogIndex newIndex, std::shared_ptr<QuorumData>);

    auto getLogIterator(LogIndex from) -> std::shared_ptr<LogIterator>;

    void sendAppendEntries(Follower&);

    void persistRemainingLogEntries();

    std::variant<Unconfigured, LeaderConfig, FollowerConfig> _role;
    ParticipantId _id{};
    std::shared_ptr<PersistedLog> _persistedLog;
    // last *valid* entry
    LogIndex _persistedLogEnd{0};
    LogTerm _currentTerm = LogTerm{};
    std::deque<LogEntry> _log;
    std::shared_ptr<InMemoryState> _state;
    LogIndex _commitIndex{0};
    std::shared_ptr<QuorumData> _lastQuorum;
    std::multimap<LogIndex, WaitForPromise> _waitForQueue;
  };

  // make this thread safe in the most simple way possible, wrap everything in
  // a single mutex.
  Guarded<GuardedInMemoryLog> _joermungandr;

  auto acquireMutex() -> Guard;
  auto acquireMutex() const -> ConstGuard;
};

struct DelayedFollowerLog : InMemoryLog {
  using InMemoryLog::InMemoryLog;
  auto appendEntries(AppendEntriesRequest) -> arangodb::futures::Future<AppendEntriesResult> override;
  void runAsyncAppendEntries();

  [[nodiscard]] auto hasPendingAppendEntries() const -> bool { return !_asyncQueue.empty(); }
 private:
  using WaitForAsyncPromise = futures::Promise<arangodb::futures::Unit>;
  std::vector<WaitForAsyncPromise> _asyncQueue;
};

}  // namespace arangodb::replication2

#endif  // ARANGOD_REPLICATION2_INMEMORYLOG_H
