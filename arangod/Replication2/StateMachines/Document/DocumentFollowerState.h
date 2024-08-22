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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Replication2/StateMachines/Document/DocumentCore.h"
#include "Replication2/StateMachines/Document/DocumentStateErrorHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateTransactionHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateSnapshot.h"
#include "Replication2/StateMachines/Document/LowestSafeIndexesForReplay.h"
#include "Replication2/StateMachines/Document/ReplicatedOperation.h"

#include "Actor/LocalActorPID.h"
#include "Basics/UnshackledMutex.h"

#include <function2.hpp>

namespace arangodb::actor {
struct LocalRuntime;
}

namespace arangodb::replication2::replicated_state::document {
namespace actor {
template<typename Runtime>
struct ApplyEntriesHandler;
}

struct IDocumentStateLeaderInterface;
struct IDocumentStateNetworkHandler;
struct SnapshotBatch;

struct DocumentFollowerState
    : replicated_state::IReplicatedFollowerState<DocumentState>,
      std::enable_shared_from_this<DocumentFollowerState> {
  explicit DocumentFollowerState(
      std::unique_ptr<DocumentCore> core, std::shared_ptr<Stream> stream,
      std::shared_ptr<IDocumentStateHandlersFactory> const& handlersFactory,
      std::shared_ptr<IScheduler> scheduler);

  ~DocumentFollowerState() override;

  friend struct actor::ApplyEntriesHandler<arangodb::actor::LocalRuntime>;

  GlobalLogIdentifier const gid;
  LoggerContext const loggerContext;

  auto getAssociatedShardList() const -> std::vector<ShardID>;

 protected:
  [[nodiscard]] auto resign() && noexcept
      -> std::unique_ptr<DocumentCore> override;

  auto acquireSnapshot(ParticipantId const& destination) noexcept
      -> futures::Future<Result> override;

  auto applyEntries(std::unique_ptr<EntryIterator> ptr) noexcept
      -> futures::Future<Result> override;

  using replicated_state::IReplicatedFollowerState<DocumentState>::getStream;

 private:
  void shutdownRuntime() noexcept;

  struct SnapshotTransferResult {
    Result res{};
    bool reportFailure{};
    std::optional<SnapshotId> snapshotId{};
  };

  auto handleSnapshotTransfer(
      std::optional<SnapshotId> snapshotId,
      std::shared_ptr<IDocumentStateLeaderInterface> leader,
      std::uint64_t snapshotVersion,
      futures::Future<ResultT<SnapshotBatch>>&& snapshotFuture) noexcept
      -> futures::Future<SnapshotTransferResult>;

  struct GuardedData {
    explicit GuardedData(std::unique_ptr<DocumentCore> core,
                         LoggerContext const& loggerContext);

    LoggerContext const& loggerContext;

    [[nodiscard]] bool didResign() const noexcept { return core == nullptr; }

    std::unique_ptr<DocumentCore> core;
    std::uint64_t currentSnapshotVersion;
  };

  std::shared_ptr<IDocumentStateNetworkHandler> const _networkHandler;
  std::shared_ptr<IDocumentStateShardHandler> const _shardHandler;
  std::shared_ptr<IDocumentStateTransactionHandler> const _transactionHandler;
  std::shared_ptr<IDocumentStateErrorHandler> const _errorHandler;

  Guarded<GuardedData, basics::UnshackledMutex> _guardedData;

  std::atomic<bool> _resigning{false};  // Allows for a quicker shutdown of
                                        // the state machine upon resigning

  std::shared_ptr<arangodb::actor::LocalRuntime> _runtime;
  arangodb::actor::LocalActorPID _applyEntriesActor;

  Guarded<LowestSafeIndexesForReplay> _lowestSafeIndexesForReplay;
};

}  // namespace arangodb::replication2::replicated_state::document
