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

#include "Futures/Future.h"

#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "Replication2/ReplicatedState/StateInterfaces.h"
#include "Replication2/Streams/Streams.h"

namespace arangodb::replication2 {
namespace test {

struct MyCoreType;
struct TestCoreType {};

/**
 * Empty follower. Always immediately responds with success.
 * @tparam S state description
 */
template<typename S>
struct EmptyFollowerType : replicated_state::IReplicatedFollowerState<S> {
  using EntryIterator =
      typename replicated_state::IReplicatedFollowerState<S>::EntryIterator;

  using CoreType =
      typename replicated_state::IReplicatedFollowerState<S>::CoreType;
  using Stream = typename replicated_state::IReplicatedFollowerState<S>::Stream;
  explicit EmptyFollowerType(std::unique_ptr<CoreType> core,
                             std::shared_ptr<Stream> stream)
      : replicated_state::IReplicatedFollowerState<S>(std::move(stream)),
        _core(std::move(core)) {}

  [[nodiscard]] auto resign() && noexcept -> std::unique_ptr<CoreType> override;

 protected:
  auto applyEntries(std::unique_ptr<EntryIterator>) noexcept
      -> futures::Future<Result> override {
    return futures::Future<Result>{std::in_place};
  }
  auto acquireSnapshot(ParticipantId const&) noexcept
      -> futures::Future<Result> override {
    return futures::Future<Result>{std::in_place};
  }

  std::unique_ptr<CoreType> _core;
};

template<typename S>
auto EmptyFollowerType<S>::resign() && noexcept -> std::unique_ptr<CoreType> {
  return std::move(_core);
}

/**
 * Empty leader. Always immediately responds with success.
 * @tparam S
 */
template<typename S>
struct EmptyLeaderType : replicated_state::IReplicatedLeaderState<S> {
  using EntryIterator =
      typename replicated_state::IReplicatedLeaderState<S>::EntryIterator;
  using Stream = typename replicated_state::IReplicatedLeaderState<S>::Stream;

  explicit EmptyLeaderType(std::unique_ptr<test::TestCoreType> core,
                           std::shared_ptr<Stream> stream)
      : replicated_state::IReplicatedLeaderState<S>(std::move(stream)),
        _core(std::move(core)) {}

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<test::TestCoreType> override;

  ~EmptyLeaderType() { TRI_ASSERT(_core == nullptr); }

 protected:
  auto recoverEntries(std::unique_ptr<EntryIterator> ptr)
      -> futures::Future<Result> override {
    return futures::Future<Result>(std::in_place);
  }

  std::unique_ptr<test::TestCoreType> _core;
};

template<typename S>
auto EmptyLeaderType<S>::resign() && noexcept
    -> std::unique_ptr<test::TestCoreType> {
  return std::move(_core);
}

template<typename Input, typename Result>
struct AsyncOperationMarker {
  ~AsyncOperationMarker() {
    TRI_ASSERT(!promise.has_value() or promise->isFulfilled())
        << "unfulfilled promise in " << ADB_HERE;
  }

  auto trigger(Input inValue) -> futures::Future<Result> {
    TRI_ASSERT(in.has_value() == false);
    in.emplace(std::move(inValue));
    triggered = true;
    return promise.emplace().getFuture();
  }

  auto resolveWith(Result res) {
    TRI_ASSERT(triggered);
    TRI_ASSERT(!promise->isFulfilled());
    promise->setValue(std::move(res));
  }

  auto resolveWithAndReset(Result res) {
    TRI_ASSERT(triggered);
    TRI_ASSERT(!promise->isFulfilled());
    auto p = std::move(promise);
    reset();
    std::move(p)->setValue(std::move(res));
  }

  auto inspectValue() const -> Input const& { return *in; }

  [[nodiscard]] auto wasTriggered() const noexcept -> bool { return triggered; }

  void reset() {
    TRI_ASSERT(triggered);
    TRI_ASSERT(promise->isFulfilled());
    triggered = false;
    in.reset();
    promise.reset();
  }

 private:
  bool triggered = false;
  std::optional<Input> in;
  std::optional<futures::Promise<Result>> promise;
};

template<typename S>
struct FakeLeaderType : replicated_state::IReplicatedLeaderState<S> {
  using EntryIterator =
      typename replicated_state::IReplicatedLeaderState<S>::EntryIterator;
  using CoreType =
      typename replicated_state::IReplicatedLeaderState<S>::CoreType;
  using Stream = typename replicated_state::IReplicatedLeaderState<S>::Stream;

  explicit FakeLeaderType(std::unique_ptr<CoreType> core,
                          std::shared_ptr<Stream> stream)
      : replicated_state::IReplicatedLeaderState<S>(std::move(stream)),
        _core(std::move(core)) {}

  [[nodiscard]] auto hasReceivedRecovery() const noexcept -> bool {
    return recovery.wasTriggered();
  }

  void resolveRecoveryOk() noexcept { resolveRecovery(Result{}); }

  void resolveRecovery(Result res) noexcept {
    recovery.resolveWith(std::move(res));
  }

  AsyncOperationMarker<std::unique_ptr<EntryIterator>, Result> recovery;

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<CoreType> override {
    return std::move(_core);
  }

 protected:
  auto recoverEntries(std::unique_ptr<EntryIterator> iter)
      -> futures::Future<Result> override {
    return recovery.trigger(std::move(iter));
  }

  std::unique_ptr<CoreType> _core;
};

template<typename S>
struct FakeFollowerType : replicated_state::IReplicatedFollowerState<S> {
  using EntryIterator =
      typename replicated_state::IReplicatedFollowerState<S>::EntryIterator;
  using Stream = typename replicated_state::IReplicatedFollowerState<S>::Stream;

  explicit FakeFollowerType(std::unique_ptr<test::TestCoreType> core,
                            std::shared_ptr<Stream> stream)
      : replicated_state::IReplicatedFollowerState<S>(std::move(stream)),
        _core(std::move(core)) {}

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<test::TestCoreType> override;

  AsyncOperationMarker<std::unique_ptr<EntryIterator>, Result> apply;
  AsyncOperationMarker<ParticipantId, Result> acquire;

  using replicated_state::IReplicatedFollowerState<S>::getStream;

 protected:
  auto applyEntries(std::unique_ptr<EntryIterator> ptr) noexcept
      -> futures::Future<Result> override {
    return apply.trigger(std::move(ptr));
  }
  auto acquireSnapshot(ParticipantId const& leader) noexcept
      -> futures::Future<Result> override {
    return acquire.trigger(leader);
  }

  std::unique_ptr<test::TestCoreType> _core;
};

template<typename S>
auto FakeFollowerType<S>::resign() && noexcept
    -> std::unique_ptr<test::TestCoreType> {
  return std::move(_core);
}

/**
 * DefaultFactory simply makes the LeaderType or FollowerType shared.
 * @tparam LeaderType
 * @tparam FollowerType
 */
template<typename LeaderType, typename FollowerType>
struct DefaultFactory {
  using CoreType = typename LeaderType::CoreType;
  auto constructLeader(std::unique_ptr<CoreType> core,
                       std::shared_ptr<typename LeaderType::Stream> stream)
      -> std::shared_ptr<LeaderType> {
    return std::make_shared<LeaderType>(std::move(core), std::move(stream));
  }
  auto constructFollower(std::unique_ptr<CoreType> core,
                         std::shared_ptr<typename FollowerType::Stream> stream,
                         std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<FollowerType> {
    return std::make_shared<FollowerType>(std::move(core), std::move(stream));
  }
  auto constructCore(TRI_vocbase_t&, GlobalLogIdentifier const&)
      -> std::unique_ptr<CoreType> {
    return std::make_unique<CoreType>();
  }
};

/**
 * By design the replicated state manager do not make the state implementations
 * leader and follower available to the outside world until a certain stage in
 * startup procedure is reached.
 *
 * This recording factory keeps track of all created instances and allow a
 * tester to access them immediately after creation. It only stores weak
 * pointer, otherwise constructors might not run until the factory is destroyed.
 * @tparam LeaderType
 * @tparam FollowerType
 */
template<typename LeaderType, typename FollowerType>
struct RecordingFactory {
  static_assert(std::is_same_v<typename LeaderType::CoreType,
                               typename FollowerType::CoreType>);
  using CoreType = typename LeaderType::CoreType;
  auto constructLeader(std::unique_ptr<CoreType> core,
                       std::shared_ptr<typename LeaderType::Stream> stream)
      -> std::shared_ptr<LeaderType> {
    auto ptr = std::make_shared<LeaderType>(std::move(core), std::move(stream));
    leaders.push_back(ptr);
    return ptr;
  }
  auto constructFollower(std::unique_ptr<CoreType> core,
                         std::shared_ptr<typename FollowerType::Stream> stream,
                         std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<FollowerType> {
    auto ptr =
        std::make_shared<FollowerType>(std::move(core), std::move(stream));
    followers.push_back(ptr);
    return ptr;
  }

  auto constructCore(TRI_vocbase_t&, GlobalLogIdentifier const&)
      -> std::unique_ptr<CoreType> {
    return std::make_unique<CoreType>();
  }

  auto getLatestLeader() -> std::shared_ptr<LeaderType> {
    TRI_ASSERT(!leaders.empty());
    return leaders.back().lock();
  }

  auto getLatestFollower() -> std::shared_ptr<FollowerType> {
    TRI_ASSERT(!followers.empty());
    return followers.back().lock();
  }

  std::vector<std::weak_ptr<LeaderType>> leaders;
  std::vector<std::weak_ptr<FollowerType>> followers;
};

/**
 * DefaultEntryType contains a key and value. Should be enough for most tests.
 */
struct DefaultEntryType {
  std::string key, value;
};
}  // namespace test

template<>
struct replicated_state::EntryDeserializer<test::DefaultEntryType> {
  auto operator()(streams::serializer_tag_t<test::DefaultEntryType>,
                  velocypack::Slice s) const -> test::DefaultEntryType;
};

template<>
struct replicated_state::EntrySerializer<test::DefaultEntryType> {
  void operator()(streams::serializer_tag_t<test::DefaultEntryType>,
                  test::DefaultEntryType const& e,
                  velocypack::Builder& b) const;
};
}  // namespace arangodb::replication2
