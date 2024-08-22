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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "MethodsProvider.h"

#include "Replication2/ReplicatedLog/Components/IFollowerCommitManager.h"
#include "Replication2/ReplicatedLog/Components/IStorageManager.h"
#include "Replication2/ReplicatedLog/Components/ISnapshotManager.h"
#include "Replication2/ReplicatedLog/Components/ICompactionManager.h"
#include "Replication2/ReplicatedLog/Components/IMessageIdManager.h"
#include "Replication2/ReplicatedLog/Components/StateMetadataTransaction.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"

namespace arangodb {
namespace replication2 {
namespace replicated_log {

struct FollowerMethodsImpl : IReplicatedLogFollowerMethods {
  FollowerMethodsImpl(std::shared_ptr<IFollowerCommitManager> commit,
                      std::shared_ptr<IStorageManager> storage,
                      std::shared_ptr<ICompactionManager> compaction,
                      std::shared_ptr<ISnapshotManager> snapshot,
                      std::shared_ptr<IMessageIdManager> messageIdManager)
      : commit(std::move(commit)),
        storage(std::move(storage)),
        compaction(std::move(compaction)),
        snapshot(std::move(snapshot)),
        messageIdManager(std::move(messageIdManager)) {}

  auto releaseIndex(LogIndex index) -> void override {
    compaction->updateReleaseIndex(index);
  }

  auto getCommittedLogIterator(std::optional<LogRange> range)
      -> std::unique_ptr<LogViewRangeIterator> override {
    return storage->getCommittedLogIterator(range);
  }

  auto waitFor(LogIndex index) -> ILogParticipant::WaitForFuture override {
    return commit->waitFor(index);
  }

  auto waitForIterator(LogIndex index)
      -> ILogParticipant::WaitForIteratorFuture override {
    return commit->waitForIterator(index);
  }

  auto snapshotCompleted(std::uint64_t version) -> Result override {
    return snapshot->setSnapshotStateAvailable(
        messageIdManager->getLastReceivedMessageId(), version);
  }

  [[nodiscard]] auto leaderConnectionEstablished() const -> bool override {
    // Having a commit index means we've got at least one append entries request
    // which was also applied *successfully*.
    // Note that this is pessimistic, in the sense that it actually waits for an
    // append entries request that was sent after leadership was established,
    // which we don't necessarily need.
    return commit->getCommitIndex() > LogIndex{0};
  }

  auto checkSnapshotState() const noexcept
      -> replicated_log::SnapshotState override {
    return snapshot->checkSnapshotState();
  }

  auto beginMetadataTrx()
      -> std::unique_ptr<IStateMetadataTransaction> override {
    return std::make_unique<StateMetadataTransaction>(
        storage->beginMetaInfoTrx());
  }
  auto commitMetadataTrx(std::unique_ptr<IStateMetadataTransaction> ptr)
      -> Result override {
    auto& trx = dynamic_cast<StateMetadataTransaction&>(*ptr);
    return storage->commitMetaInfoTrx(std::move(trx.trx));
  }
  auto getCommittedMetadata() const
      -> IStateMetadataTransaction::DataType override {
    return storage->getCommittedMetaInfo().stateOwnedMetadata;
  }

 private:
  std::shared_ptr<IFollowerCommitManager> const commit;
  std::shared_ptr<IStorageManager> const storage;
  std::shared_ptr<ICompactionManager> const compaction;
  std::shared_ptr<ISnapshotManager> const snapshot;
  std::shared_ptr<IMessageIdManager> const messageIdManager;
};

MethodsProviderManager::MethodsProviderManager(
    std::shared_ptr<IFollowerCommitManager> commit,
    std::shared_ptr<IStorageManager> storage,
    std::shared_ptr<ICompactionManager> compaction,
    std::shared_ptr<ISnapshotManager> snapshot,
    std::shared_ptr<IMessageIdManager> messageIdManager)
    : commit(std::move(commit)),
      storage(std::move(storage)),
      compaction(std::move(compaction)),
      snapshot(std::move(snapshot)),
      messageIdManager(std::move(messageIdManager)) {}

auto MethodsProviderManager::getMethods()
    -> std::unique_ptr<IReplicatedLogFollowerMethods> {
  return std::make_unique<FollowerMethodsImpl>(commit, storage, compaction,
                                               snapshot, messageIdManager);
}

}  // namespace replicated_log
}  // namespace replication2
}  // namespace arangodb