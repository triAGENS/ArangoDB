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

#ifndef ARANGOD_REPLICATION2_REPLICATEDLOG_H
#define ARANGOD_REPLICATION2_REPLICATEDLOG_H

#include <Basics/Guarded.h>
#include <Futures/Future.h>
#include <velocypack/Builder.h>
#include <velocypack/SharedSlice.h>

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/map.hpp>
#include <immer/flex_vector.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
  immer::flex_vector<LogEntry> entries;

  void toVelocyPack(velocypack::Builder& builder) const;
  static auto fromVelocyPack(velocypack::Slice slice) -> AppendEntriesRequest;
};

/**
 * @brief State stub, later to be replaced by a RocksDB state.
 * Currently unused.
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
 * @brief A simple replicated log implementation, mainly for prototyping
 * replication 2.0.
 */
class ReplicatedLog : public LogFollower,
                      public std::enable_shared_from_this<ReplicatedLog> {
 public:
  ReplicatedLog() = delete;
  explicit ReplicatedLog(ParticipantId participantId, std::shared_ptr<InMemoryState> state,
                         std::shared_ptr<PersistedLog> persistedLog);

  // follower only
  auto appendEntries(AppendEntriesRequest)
      -> arangodb::futures::Future<AppendEntriesResult> override;

  // leader only
  auto insert(LogPayload) -> LogIndex;

  // leader only
  [[nodiscard]] auto createSnapshot() const
      -> std::pair<LogIndex, std::shared_ptr<InMemoryState const>>;

  // leader only
  auto waitFor(LogIndex) -> arangodb::futures::Future<std::shared_ptr<QuorumData>>;

  // Set to follower, and (strictly increase) term to the given value
  auto becomeFollower(LogTerm, ParticipantId leaderId) -> void;

  // Set to leader, and (strictly increase) term to the given value
  auto becomeLeader(LogTerm term, std::vector<std::shared_ptr<LogFollower>> const& follower,
                    std::size_t writeConcern) -> void;

  [[nodiscard]] auto getLocalStatistics() const -> LogStatistics;
  [[nodiscard]] auto getStatus() const -> LogStatus;

  auto runAsyncStep() -> void;

  [[nodiscard]] auto participantId() const noexcept -> ParticipantId override;

  // TODO Do we want another read function that only allows to read
  //      committed entries?
  [[nodiscard]] auto getEntryByIndex(LogIndex) const -> std::optional<LogEntry>;

  [[nodiscard]] auto getReplicatedLogSnapshot() const -> immer::flex_vector<LogEntry>;

 private:
  struct GuardedReplicatedLog;
  using Guard = MutexGuard<GuardedReplicatedLog, std::unique_lock<std::mutex>>;
  using ConstGuard = MutexGuard<GuardedReplicatedLog const, std::unique_lock<std::mutex>>;

  struct Follower {
    explicit Follower(std::shared_ptr<LogFollower> impl, LogIndex lastLogIndex)
        : _impl(std::move(impl)), lastAckedIndex(lastLogIndex) {}

    std::shared_ptr<LogFollower> _impl;
    LogIndex lastAckedIndex = LogIndex{0};
    LogIndex lastAckedCommitIndex = LogIndex{0};
    bool requestInFlight = false;
    std::size_t numErrorsSinceLastAnswer = 0;
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

  struct alignas(128) GuardedReplicatedLog {
    GuardedReplicatedLog() = delete;
    GuardedReplicatedLog(GuardedReplicatedLog const&) = delete;
    GuardedReplicatedLog(GuardedReplicatedLog&&) = delete;
    auto operator=(GuardedReplicatedLog const&) -> GuardedReplicatedLog& = delete;
    auto operator=(GuardedReplicatedLog&&) -> GuardedReplicatedLog& = delete;
    ~GuardedReplicatedLog() = default;
    GuardedReplicatedLog(ParticipantId id, std::shared_ptr<InMemoryState> state,
                         std::shared_ptr<PersistedLog> persistedLog, LogIndex logIndex)
        : _id(std::move(id)),
          _persistedLog(std::move(persistedLog)),
          _state(std::move(state)),
          _commitIndex{logIndex} {}

    // follower only
    auto appendEntries(AppendEntriesRequest)
        -> arangodb::futures::Future<AppendEntriesResult>;

    // leader only
    auto insert(LogPayload) -> LogIndex;

    // leader only
    [[nodiscard]] auto createSnapshot() const
        -> std::pair<LogIndex, std::shared_ptr<InMemoryState const>>;

    // leader only
    [[nodiscard]] auto waitFor(LogIndex) -> arangodb::futures::Future<std::shared_ptr<QuorumData>>;

    // Set to follower, and (strictly increase) term to the given value
    auto becomeFollower(LogTerm, ParticipantId leaderId) -> void;

    // Set to leader, and (strictly increase) term to the given value
    auto becomeLeader(LogTerm term, std::vector<std::shared_ptr<LogFollower>> const& follower,
                      std::size_t writeConcern) -> void;

    auto handleAppendEntriesResponse(std::weak_ptr<ReplicatedLog> const& parentLog,
                                     Follower& follower, LogIndex lastIndex,
                                     LogIndex currentCommitIndex, LogTerm currentTerm,
                                     futures::Try<AppendEntriesResult>&& res) -> void;

    [[nodiscard]] auto getStatistics() const -> LogStatistics;

    auto runAsyncStep(std::weak_ptr<ReplicatedLog> const& parentLog) -> void;

    [[nodiscard]] auto participantId() const noexcept -> ParticipantId;
    [[nodiscard]] auto getEntryByIndex(LogIndex) const -> std::optional<LogEntry>;

    [[nodiscard]] auto nextIndex() const -> LogIndex;
    [[nodiscard]] auto getLastIndex() const -> LogIndex;
    void assertLeader() const;
    void assertFollower() const;

    void checkCommitIndex();
    void updateCommitIndexLeader(LogIndex newIndex,
                                 const std::shared_ptr<QuorumData>& quorum);

    [[nodiscard]] auto getLogIterator(LogIndex from) const -> std::shared_ptr<LogIterator>;

    void sendAppendEntries(std::weak_ptr<ReplicatedLog> const& parentLog, Follower&);

    std::variant<Unconfigured, LeaderConfig, FollowerConfig> _role;
    ParticipantId _id{};
    std::shared_ptr<PersistedLog> _persistedLog;
    LogTerm _currentTerm = LogTerm{};
    immer::flex_vector<LogEntry> _log{};
    std::shared_ptr<InMemoryState> _state;
    LogIndex _commitIndex{0};
    std::shared_ptr<QuorumData> _lastQuorum;
    std::multimap<LogIndex, WaitForPromise> _waitForQueue;
  };

  // make this thread safe in the most simple way possible, wrap everything in
  // a single mutex.
  Guarded<GuardedReplicatedLog> _joermungandr;

  auto acquireMutex() -> Guard;
  auto acquireMutex() const -> ConstGuard;
};

}  // namespace arangodb::replication2

#endif  // ARANGOD_REPLICATION2_REPLICATEDLOG_H
