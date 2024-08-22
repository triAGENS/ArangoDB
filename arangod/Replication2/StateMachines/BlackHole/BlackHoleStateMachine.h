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

#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "Replication2/ReplicatedState/StateInterfaces.h"
#include "Replication2/StateMachines/BlackHole/Monostate.h"

struct TRI_vocbase_t;

namespace arangodb::replication2::replicated_state {
/**
 * The black-hole state machine is here only for testing purpose. It accepts
 * all writes. Writes are replicated and then discarded. Followers do nothing,
 * except receiving log data. Snapshot transfer is always successful.
 */
namespace black_hole {

struct BlackHoleFactory;
struct BlackHoleLogEntry;
struct BlackHoleLeaderState;
struct BlackHoleFollowerState;
struct BlackHoleCore;

struct BlackHoleState {
  static constexpr std::string_view NAME = "black-hole";

  using LeaderType = BlackHoleLeaderState;
  using FollowerType = BlackHoleFollowerState;
  using EntryType = BlackHoleLogEntry;
  using FactoryType = BlackHoleFactory;
  using CoreType = BlackHoleCore;
  using CoreParameterType = void;
  using CleanupHandlerType = void;
  using MetadataType = Monostate;
};

struct BlackHoleLogEntry {
  [[nodiscard]] static auto createFromString(std::string_view payload)
      -> BlackHoleLogEntry {
    return BlackHoleLogEntry{.value = LogPayload::createFromString(payload)};
  }

  [[nodiscard]] static auto createFromSlice(velocypack::Slice slice)
      -> BlackHoleLogEntry {
    return BlackHoleLogEntry{.value = LogPayload::createFromSlice(slice)};
  }
  LogPayload value;
};

struct BlackHoleLeaderState
    : replicated_state::IReplicatedLeaderState<BlackHoleState> {
  explicit BlackHoleLeaderState(std::unique_ptr<BlackHoleCore> core,
                                std::shared_ptr<Stream> stream);
  auto write(std::string_view) -> LogIndex;

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<BlackHoleCore> override;

  auto release(LogIndex) const -> futures::Future<Result>;
  auto insert(LogPayload payload, bool waitForSync = false) -> LogIndex;

 protected:
  auto recoverEntries(std::unique_ptr<EntryIterator> ptr)
      -> futures::Future<Result> override;

  std::unique_ptr<BlackHoleCore> _core;
};

struct BlackHoleFollowerState
    : replicated_state::IReplicatedFollowerState<BlackHoleState> {
  explicit BlackHoleFollowerState(std::unique_ptr<BlackHoleCore> core,
                                  std::shared_ptr<Stream> stream);

  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<BlackHoleCore> override;

 protected:
  auto acquireSnapshot(ParticipantId const& destination) noexcept
      -> futures::Future<Result> override;
  auto applyEntries(std::unique_ptr<EntryIterator> ptr) noexcept
      -> futures::Future<Result> override;

  std::unique_ptr<BlackHoleCore> _core;
};

struct BlackHoleCore {};

struct BlackHoleFactory {
  auto constructFollower(
      std::unique_ptr<BlackHoleCore> core,
      std::shared_ptr<streams::Stream<BlackHoleState>> stream,
      std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<BlackHoleFollowerState>;
  auto constructLeader(
      std::unique_ptr<BlackHoleCore> core,
      std::shared_ptr<streams::ProducerStream<BlackHoleState>> stream)
      -> std::shared_ptr<BlackHoleLeaderState>;
  auto constructCore(TRI_vocbase_t&, GlobalLogIdentifier const&)
      -> std::unique_ptr<BlackHoleCore>;
};

}  // namespace black_hole

template<>
struct EntryDeserializer<black_hole::BlackHoleLogEntry> {
  auto operator()(streams::serializer_tag_t<
                      replicated_state::black_hole::BlackHoleLogEntry>,
                  velocypack::Slice s) const
      -> replicated_state::black_hole::BlackHoleLogEntry;
};

template<>
struct EntrySerializer<black_hole::BlackHoleLogEntry> {
  void operator()(streams::serializer_tag_t<
                      replicated_state::black_hole::BlackHoleLogEntry>,
                  replicated_state::black_hole::BlackHoleLogEntry const& e,
                  velocypack::Builder& b) const;
};

extern template struct replicated_state::ReplicatedState<
    black_hole::BlackHoleState>;
}  // namespace arangodb::replication2::replicated_state
