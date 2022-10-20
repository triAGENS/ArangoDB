////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Replication2/DeferredExecution.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedState/ReplicatedStateToken.h"
#include "Replication2/ReplicatedState/ReplicatedStateTraits.h"
#include "Replication2/ReplicatedState/StateStatus.h"
#include "Replication2/ReplicatedState/WaitForQueue.h"
#include "Replication2/Streams/Streams.h"

#include "Basics/Guarded.h"

#include <map>

namespace arangodb {
class Result;

namespace futures {
template<typename T>
class Future;
template<typename T>
class Promise;
struct Unit;
}  // namespace futures

namespace velocypack {
class SharedSlice;
}

}  // namespace arangodb

namespace arangodb::replication2 {
namespace replicated_log {
struct ReplicatedLog;
struct ILogFollower;
struct ILogLeader;
struct ILogParticipant;
struct LogUnconfiguredParticipant;
}  // namespace replicated_log

namespace replicated_state {
struct StatePersistorInterface;
struct ReplicatedStateMetrics;
struct IReplicatedLeaderStateBase;
struct IReplicatedFollowerStateBase;

struct IStateManagerBase {};
template<typename S>
struct IReplicatedStateImplBase;
template<typename S>
struct IReplicatedFollowerState;
template<typename S>
struct IReplicatedLeaderState;

template<typename S>
using ReplicatedStateStreamSpec =
    streams::stream_descriptor_set<streams::stream_descriptor<
        streams::StreamId{1}, typename ReplicatedStateTraits<S>::EntryType,
        streams::tag_descriptor_set<streams::tag_descriptor<
            1, typename ReplicatedStateTraits<S>::Deserializer,
            typename ReplicatedStateTraits<S>::Serializer>>>>;

/**
 * Common base class for all ReplicatedStates, hiding the type information.
 */
struct ReplicatedStateBase {
  virtual ~ReplicatedStateBase() = default;

  virtual void drop(
      std::shared_ptr<replicated_log::IReplicatedStateHandle>) && = 0;
  [[nodiscard]] virtual auto getStatus() -> std::optional<StateStatus> = 0;
  [[nodiscard]] auto getLeader()
      -> std::shared_ptr<IReplicatedLeaderStateBase> {
    return getLeaderBase();
  }
  [[nodiscard]] auto getFollower()
      -> std::shared_ptr<IReplicatedFollowerStateBase> {
    return getFollowerBase();
  }

  [[nodiscard]] virtual auto createStateHandle(
      std::optional<velocypack::SharedSlice> const& coreParameters)
      -> std::unique_ptr<replicated_log::IReplicatedStateHandle> = 0;

 private:
  [[nodiscard]] virtual auto getLeaderBase()
      -> std::shared_ptr<IReplicatedLeaderStateBase> = 0;
  [[nodiscard]] virtual auto getFollowerBase()
      -> std::shared_ptr<IReplicatedFollowerStateBase> = 0;
};

// TODO Clean this up, starting with trimming Stream to its minimum
template<typename EntryType, typename Deserializer,
         template<typename> typename Interface = streams::Stream,
         typename ILogMethodsT = replicated_log::IReplicatedLogMethodsBase>
struct StreamProxy : Interface<EntryType> {
  using WaitForResult = typename streams::Stream<EntryType>::WaitForResult;
  using Iterator = typename streams::Stream<EntryType>::Iterator;

 protected:
  std::unique_ptr<ILogMethodsT> _logMethods;

 public:
  explicit StreamProxy(std::unique_ptr<ILogMethodsT> methods)
      : _logMethods(std::move(methods)) {}

  auto waitFor(LogIndex index) -> futures::Future<WaitForResult> override {
    // TODO As far as I can tell right now, we can get rid of this:
    //      Delete this, also in streams::Stream.
    return _logMethods->waitFor(index).thenValue(
        [](auto const&) { return WaitForResult(); });
  }
  auto waitForIterator(LogIndex index)
      -> futures::Future<std::unique_ptr<Iterator>> override;

  auto release(LogIndex index) -> void override {
    // TODO Implement this
    std::abort();
  }
};

template<typename EntryType, typename Deserializer, typename Serializer>
struct ProducerStreamProxy
    : StreamProxy<EntryType, Deserializer, streams::ProducerStream,
                  replicated_log::IReplicatedLogLeaderMethods> {
  explicit ProducerStreamProxy(
      std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods> methods)
      : StreamProxy<EntryType, Deserializer, streams::ProducerStream,
                    replicated_log::IReplicatedLogLeaderMethods>(
            std::move(methods)) {
    ADB_PROD_ASSERT(this->_logMethods != nullptr);
  }

  // TODO waitForSync parameter is missing
  auto insert(EntryType const& v) -> LogIndex override {
    ADB_PROD_ASSERT(this->_logMethods != nullptr);
    return this->_logMethods->insert(serialize(v));
  }

  auto insertDeferred(EntryType const& v)
      -> std::pair<LogIndex, DeferredAction> override {
    // TODO Remove this method, it should be superfluous
    return this->_logMethods->insertDeferred(serialize(v));
  }

  auto methods() -> auto& { return *this->_logMethods; }

  auto resign() && -> decltype(this->_logMethods) {
    return std::move(this->_logMethods);
  }

 private:
  auto serialize(EntryType const& v) -> LogPayload {
    auto builder = velocypack::Builder();
    std::invoke(Serializer{}, streams::serializer_tag<EntryType>, v, builder);
    // TODO avoid the copy
    auto payload = LogPayload::createFromSlice(builder.slice());
    return payload;
  }
};

template<typename S>
struct NewLeaderStateManager
    : std::enable_shared_from_this<NewLeaderStateManager<S>> {
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using Deserializer = typename ReplicatedStateTraits<S>::Deserializer;
  using Serializer = typename ReplicatedStateTraits<S>::Serializer;
  explicit NewLeaderStateManager(
      LoggerContext loggerContext,
      std::shared_ptr<ReplicatedStateMetrics> metrics,
      std::shared_ptr<IReplicatedLeaderState<S>> leaderState,
      std::shared_ptr<ProducerStreamProxy<EntryType, Deserializer, Serializer>>
          stream);

  void recoverEntries();
  void updateCommitIndex(LogIndex index) noexcept;
  [[nodiscard]] auto resign() && noexcept
      -> std::pair<std::unique_ptr<CoreType>,
                   std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>>;
  [[nodiscard]] auto getStatus() const -> StateStatus;

  [[nodiscard]] auto getStateMachine() const
      -> std::shared_ptr<IReplicatedLeaderState<S>>;

 private:
  LoggerContext const _loggerContext;
  std::shared_ptr<ReplicatedStateMetrics> const _metrics;
  struct GuardedData {
    auto recoverEntries();
    auto getResolvablePromises(LogIndex index) noexcept -> WaitForQueue;
    [[nodiscard]] auto resign() && noexcept -> std::pair<
        std::unique_ptr<CoreType>,
        std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>>;

    LoggerContext const& _loggerContext;
    ReplicatedStateMetrics const& _metrics;
    std::shared_ptr<IReplicatedLeaderState<S>> _leaderState;
    std::shared_ptr<ProducerStreamProxy<EntryType, Deserializer, Serializer>>
        _stream;
    WaitForQueue _waitQueue;
  };
  Guarded<GuardedData> _guardedData;
};

template<typename S>
struct NewFollowerStateManager
    : std::enable_shared_from_this<NewFollowerStateManager<S>> {
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using Deserializer = typename ReplicatedStateTraits<S>::Deserializer;

  explicit NewFollowerStateManager(
      LoggerContext loggerContext,
      std::shared_ptr<ReplicatedStateMetrics> metrics,
      std::shared_ptr<IReplicatedFollowerState<S>> followerState,
      std::unique_ptr<replicated_log::IReplicatedLogFollowerMethods>
          logMethods);
  void acquireSnapshot(ServerID leader, LogIndex index);
  void updateCommitIndex(LogIndex index);
  [[nodiscard]] auto resign() && noexcept
      -> std::pair<std::unique_ptr<CoreType>,
                   std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>>;
  [[nodiscard]] auto getStatus() const -> StateStatus;

  [[nodiscard]] auto getStateMachine() const
      -> std::shared_ptr<IReplicatedFollowerState<S>>;

 private:
  LoggerContext const _loggerContext;
  std::shared_ptr<ReplicatedStateMetrics> const _metrics;
  struct GuardedData {
    auto updateCommitIndex(LogIndex index)
        -> std::optional<futures::Future<Result>>;

    std::shared_ptr<IReplicatedFollowerState<S>> _followerState;
    std::unique_ptr<replicated_log::IReplicatedLogFollowerMethods> _logMethods;
    LogIndex _commitIndex = LogIndex{0};
    LogIndex _lastAppliedIndex = LogIndex{0};
    std::optional<LogIndex> _applyEntriesIndexInFlight = std::nullopt;
  };
  Guarded<GuardedData> _guardedData;
};

template<typename S>
struct NewUnconfiguredStateManager
    : std::enable_shared_from_this<NewUnconfiguredStateManager<S>> {
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  explicit NewUnconfiguredStateManager(LoggerContext loggerContext,
                                       std::unique_ptr<CoreType>) noexcept;
  [[nodiscard]] auto resign() && noexcept
      -> std::pair<std::unique_ptr<CoreType>,
                   std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>>;
  [[nodiscard]] auto getStatus() const -> StateStatus;

 private:
  LoggerContext const _loggerContext;
  struct GuardedData {
    [[nodiscard]] auto resign() && noexcept -> std::unique_ptr<CoreType>;

    std::unique_ptr<CoreType> _core;
  };
  Guarded<GuardedData> _guardedData;
};

template<typename S>
struct ReplicatedStateManager : replicated_log::IReplicatedStateHandle {
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;
  using Factory = typename ReplicatedStateTraits<S>::FactoryType;
  using FollowerType = typename ReplicatedStateTraits<S>::FollowerType;
  using LeaderType = typename ReplicatedStateTraits<S>::LeaderType;
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using Serializer = typename ReplicatedStateTraits<S>::Serializer;
  using Deserializer = typename ReplicatedStateTraits<S>::Deserializer;

  ReplicatedStateManager(LoggerContext loggerContext,
                         std::shared_ptr<ReplicatedStateMetrics> metrics,
                         std::unique_ptr<CoreType> logCore,
                         std::shared_ptr<Factory> factory);

  void acquireSnapshot(ServerID leader, LogIndex index) override;

  void updateCommitIndex(LogIndex index) override;

  [[nodiscard]] auto resignCurrentState() noexcept
      -> std::unique_ptr<replicated_log::IReplicatedLogMethodsBase> override;

  void leadershipEstablished(
      std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods>) override;

  void becomeFollower(
      std::unique_ptr<replicated_log::IReplicatedLogFollowerMethods> methods)
      override;

  void dropEntries() override;

  auto resign() && -> std::unique_ptr<CoreType>;

  [[nodiscard]] auto getStatus() const -> std::optional<StateStatus> override;
  // We could, more specifically, return pointers to FollowerType/LeaderType.
  // But I currently don't see that it's needed, and would have to do one of
  // the stunts for covariance here.
  [[nodiscard]] auto getFollower() const
      -> std::shared_ptr<IReplicatedFollowerStateBase> override;
  [[nodiscard]] auto getLeader() const
      -> std::shared_ptr<IReplicatedLeaderStateBase> override;

 private:
  LoggerContext const _loggerContext;
  std::shared_ptr<ReplicatedStateMetrics> const _metrics;
  std::shared_ptr<Factory> const _factory;

  struct GuardedData {
    template<typename... Args>
    explicit GuardedData(Args&&... args)
        : _currentManager(std::forward<Args>(args)...) {}

    std::variant<std::shared_ptr<NewUnconfiguredStateManager<S>>,
                 std::shared_ptr<NewLeaderStateManager<S>>,
                 std::shared_ptr<NewFollowerStateManager<S>>>
        _currentManager;
  };
  Guarded<GuardedData> _guarded;
};

template<typename S>
struct ReplicatedState final
    : ReplicatedStateBase,
      std::enable_shared_from_this<ReplicatedState<S>> {
  using Factory = typename ReplicatedStateTraits<S>::FactoryType;
  using EntryType = typename ReplicatedStateTraits<S>::EntryType;
  using FollowerType = typename ReplicatedStateTraits<S>::FollowerType;
  using LeaderType = typename ReplicatedStateTraits<S>::LeaderType;
  using CoreType = typename ReplicatedStateTraits<S>::CoreType;

  explicit ReplicatedState(GlobalLogIdentifier gid,
                           std::shared_ptr<replicated_log::ReplicatedLog> log,
                           std::shared_ptr<Factory> factory,
                           LoggerContext loggerContext,
                           std::shared_ptr<ReplicatedStateMetrics>);
  ~ReplicatedState() override;

  void drop(std::shared_ptr<replicated_log::IReplicatedStateHandle>) &&
      override;
  /**
   * Returns the follower state machine. Returns nullptr if no follower state
   * machine is present. (i.e. this server is not a follower)
   */
  [[nodiscard]] auto getFollower() const -> std::shared_ptr<FollowerType>;
  /**
   * Returns the leader state machine. Returns nullptr if no leader state
   * machine is present. (i.e. this server is not a leader)
   */
  [[nodiscard]] auto getLeader() const -> std::shared_ptr<LeaderType>;

  [[nodiscard]] auto getStatus() -> std::optional<StateStatus> final;

  auto createStateHandle(
      std::optional<velocypack::SharedSlice> const& coreParameter)
      -> std::unique_ptr<replicated_log::IReplicatedStateHandle> override;

  struct IStateManager : IStateManagerBase {
    virtual ~IStateManager() = default;
    virtual void run() = 0;

    using WaitForAppliedPromise = futures::Promise<futures::Unit>;
    using WaitForAppliedQueue = std::multimap<LogIndex, WaitForAppliedPromise>;

    [[nodiscard]] virtual auto getStatus() const -> StateStatus = 0;
    [[nodiscard]] virtual auto resign() && noexcept
        -> std::tuple<std::unique_ptr<CoreType>,
                      std::unique_ptr<ReplicatedStateToken>,
                      DeferredAction> = 0;

    virtual auto resign2() && noexcept -> std::tuple<
        std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods>,
        std::unique_ptr<CoreType>> = 0;
  };

 private:
  auto buildCore(std::optional<velocypack::SharedSlice> const& coreParameter);
  auto getLeaderBase() -> std::shared_ptr<IReplicatedLeaderStateBase> final {
    return getLeader();
  }
  auto getFollowerBase()
      -> std::shared_ptr<IReplicatedFollowerStateBase> final {
    return getFollower();
  }

  std::shared_ptr<Factory> const factory;
  GlobalLogIdentifier const gid;
  std::shared_ptr<replicated_log::ReplicatedLog> const log{};

  struct GuardedData {
    explicit GuardedData(ReplicatedState& self) : _self(self) {}

    ReplicatedState& _self;
    std::shared_ptr<IStateManager> currentManager = nullptr;
    std::unique_ptr<CoreType> oldCore = nullptr;
  };
  Guarded<GuardedData> guardedData;
  LoggerContext const loggerContext;
  DatabaseID const database;
  std::shared_ptr<ReplicatedStateMetrics> const metrics;
};

}  // namespace replicated_state
}  // namespace arangodb::replication2
