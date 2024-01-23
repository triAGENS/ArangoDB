////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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

#pragma once

#include "Replication2/DeferredExecution.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogEntryView.h"
#include "Replication2/ReplicatedLog/NetworkMessages.h"
#include "Basics/ResultT.h"

#include <Futures/Future.h>
#include <Futures/Promise.h>

#include <map>
#include <memory>

namespace arangodb {
class Result;
struct LoggerContext;
}  // namespace arangodb

namespace arangodb::replication2::storage {
struct IStorageEngineMethods;
}

namespace arangodb::replication2::replicated_log {

struct IReplicatedStateHandle;
struct LogStatus;
struct QuickLogStatus;
struct InMemoryLog;

struct WaitForResult {
  /// @brief contains the _current_ commit index. (Not the index waited for)
  LogIndex currentCommitIndex;
  /// @brief Quorum information
  std::shared_ptr<QuorumData const> quorum;

  WaitForResult(LogIndex index, std::shared_ptr<QuorumData const> quorum);
  WaitForResult() = default;
  explicit WaitForResult(velocypack::Slice);

  void toVelocyPack(velocypack::Builder&) const;
};

/**
 * @brief Interface for a log participant: That is, usually either a leader or a
 * follower (LogLeader and LogFollower). Can also be a
 * LogUnconfiguredParticipant, e.g. during startup. The most prominent thing
 * this interface provides is that each instance is responsible for a singular
 * LogCore, which can be moved out with resign().
 */
struct ILogParticipant {
  [[nodiscard]] virtual auto getStatus() const -> LogStatus = 0;
  [[nodiscard]] virtual auto getQuickStatus() const -> QuickLogStatus = 0;
  virtual ~ILogParticipant() = default;
  [[nodiscard]] virtual auto resign() && -> std::tuple<
      std::unique_ptr<storage::IStorageEngineMethods>,
      std::unique_ptr<IReplicatedStateHandle>, DeferredAction> = 0;

  using WaitForPromise = futures::Promise<WaitForResult>;
  using WaitForFuture = futures::Future<WaitForResult>;
  using WaitForIteratorFuture =
      futures::Future<std::unique_ptr<LogViewRangeIterator>>;
  using WaitForQueue = std::multimap<LogIndex, WaitForPromise>;

  [[nodiscard]] virtual auto waitFor(LogIndex index) -> WaitForFuture = 0;
  [[nodiscard]] virtual auto waitForIterator(LogIndex index)
      -> WaitForIteratorFuture = 0;

  // Passing no bounds means everything.
  [[nodiscard]] virtual auto getInternalLogIterator(
      std::optional<LogRange> bounds = std::nullopt) const
      -> std::unique_ptr<LogIterator> = 0;
  [[nodiscard]] virtual auto compact() -> ResultT<CompactionResult> = 0;
};

/**
 * Interface describing a LogFollower API. Components should use this interface
 * if they want to refer to a LogFollower instance.
 */
struct ILogFollower : ILogParticipant, AbstractFollower {};

struct ILeaderCommunicator {
  virtual ~ILeaderCommunicator() = default;
  virtual auto getParticipantId() const noexcept -> ParticipantId const& = 0;
  /// @param mid Last message id received from the leader. This is reported to
  ///            the leader, so it can ignore snapshot status updates from
  ///            append entries responses that are lower than or equal to this
  ///            id, as they are less recent than this information.
  virtual auto reportSnapshotAvailable(MessageId mid) noexcept
      -> futures::Future<Result> = 0;
};

/**
 * Interfaces describe a LogLeader API. Components should use this interface
 * if they want to refer to a LogLeader instance.
 */
struct ILogLeader : ILogParticipant {
  virtual auto updateParticipantsConfig(
      std::shared_ptr<agency::ParticipantsConfig const> const& config)
      -> LogIndex = 0;
  virtual auto ping(std::optional<std::string> message) -> LogIndex = 0;
  virtual auto waitForLeadership() -> WaitForFuture = 0;
};

}  // namespace arangodb::replication2::replicated_log
