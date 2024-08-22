////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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

#pragma once

#include "Replication2/ReplicatedState/ReplicatedStateTraits.h"
#include "Replication2/ReplicatedState/StateStatus.h"
#include "Replication2/ReplicatedState/WaitForQueue.h"
#include "Replication2/Streams/Streams.h"

#include <memory>

namespace arangodb::futures {
struct Unit;
template<typename T>
class Future;
}  // namespace arangodb::futures
namespace arangodb {
class Result;
}
namespace arangodb::replication2 {
namespace replicated_log {
struct ReplicatedLog;
struct ILogFollower;
struct ILogLeader;
}  // namespace replicated_log

namespace replicated_state {

template<typename S>
struct FollowerStateManager;
template<typename S>
struct LeaderStateManager;
template<typename S>
struct ReplicatedStateManager;

struct IReplicatedLeaderStateBase {
  virtual ~IReplicatedLeaderStateBase() = default;
};

struct IReplicatedFollowerStateBase {
  virtual ~IReplicatedFollowerStateBase() = default;
};

template<typename S>
struct IReplicatedStateImplBase {
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  virtual ~IReplicatedStateImplBase() = default;
  virtual auto resign() && noexcept -> std::unique_ptr<CoreType> = 0;
};

template<typename S>
struct IReplicatedLeaderState : IReplicatedStateImplBase<S>,
                                IReplicatedLeaderStateBase {
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  using Stream = streams::ProducerStream<S>;
  using EntryIterator = typename Stream::Iterator;

 protected:
  /**
   * This function is called once on a leader instance. The iterator contains
   * all log entries currently present in the replicated log. The state machine
   * manager awaits the return value. If the result is ok, the leader instance
   * is made available to the outside world.
   *
   * If the recovery fails, the server aborts.
   * @return Future to be fulfilled when recovery is done.
   */
  virtual auto recoverEntries(std::unique_ptr<EntryIterator>)
      -> futures::Future<Result> = 0;

  [[nodiscard]] auto getStream() const noexcept
      -> std::shared_ptr<Stream> const&;

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<CoreType> override = 0;

  /**
   * This hook is called after leader recovery is completed and the internal
   * state has been updated. The underlying stream is guaranteed to have been
   * initialized.
   */
  virtual void onRecoveryCompleted() noexcept {};

  explicit IReplicatedLeaderState(std::shared_ptr<Stream> stream);

 private:
  friend struct LeaderStateManager<S>;

  std::shared_ptr<Stream> const _stream;
};

template<typename S>
struct IReplicatedFollowerState : IReplicatedStateImplBase<S>,
                                  IReplicatedFollowerStateBase {
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  using Stream = streams::Stream<S>;
  using EntryIterator = typename Stream::Iterator;

  using WaitForAppliedFuture = futures::Future<futures::Unit>;
  [[nodiscard]] auto waitForApplied(LogIndex index) -> WaitForAppliedFuture;

 protected:
  /**
   * Called by the state machine manager if new log entries have been committed
   * and are ready to be applied to the state machine. The implementation
   * ensures that this function is not called again until the future returned is
   * fulfilled.
   *
   * Entries are not released after they are consumed by this function. Its the
   * state machines implementations responsibility to call release on the
   * stream.
   *
   * @return Future with Result value. If the result contains an error, the
   *    operation is retried.
   */
  virtual auto applyEntries(std::unique_ptr<EntryIterator>) noexcept
      -> futures::Future<Result> = 0;

  /**
   * Called by the state machine manager if a follower is requested to pull
   * data from the leader in order to transfer the snapshot.
   * @param leader
   * @param localCommitIndex
   * @return Future with Result value. If the result contains an error,
   *    the operation is eventually retried.
   */
  virtual auto acquireSnapshot(ParticipantId const& leader) noexcept
      -> futures::Future<Result> = 0;

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<CoreType> override = 0;

  explicit IReplicatedFollowerState(std::shared_ptr<Stream> stream);

 protected:
  [[nodiscard]] auto getStream() const noexcept
      -> std::shared_ptr<Stream> const&;

 private:
  friend struct FollowerStateManager<S>;
  friend struct ReplicatedStateManager<S>;

  void setStateManager(
      std::shared_ptr<FollowerStateManager<S>> manager) noexcept;

  std::weak_ptr<FollowerStateManager<S>> _manager;
  std::shared_ptr<Stream> const _stream;
};

}  // namespace replicated_state
}  // namespace arangodb::replication2
