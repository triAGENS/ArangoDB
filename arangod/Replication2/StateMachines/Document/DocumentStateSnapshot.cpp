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

#include "Replication2/StateMachines/Document/DocumentStateSnapshot.h"
#include "Replication2/StateMachines/Document/DocumentStateSnapshotInspectors.h"

#include "Assertions/ProdAssert.h"
#include "Replication2/StateMachines/Document/CollectionReader.h"
#include "Logger/LogContextKeys.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

namespace arangodb::replication2::replicated_state::document {
auto SnapshotId::fromString(std::string_view name) noexcept
    -> ResultT<SnapshotId> {
  auto id = basics::StringUtils::try_uint64(name);
  if (id.fail()) {
    return ResultT<SnapshotId>::error(id.result());
  }
  return ResultT<SnapshotId>::success(SnapshotId{id.get()});
}

[[nodiscard]] SnapshotId::operator velocypack::Value() const noexcept {
  return velocypack::Value(id());
}

SnapshotId SnapshotId::create() { return SnapshotId{TRI_HybridLogicalClock()}; }

auto to_string(SnapshotId snapshotId) -> std::string {
  return std::to_string(snapshotId.id());
}

std::ostream& operator<<(std::ostream& os, SnapshotBatch const& batch) {
  return os << velocypack::serialize(batch).toJson();
}

SnapshotStatus::SnapshotStatus(SnapshotState const& state,
                               SnapshotStatistics statistics)
    : statistics(std::move(statistics)) {
  this->state =
      std::visit(overload{
                     [](state::Ongoing const&) { return kStringOngoing; },
                     [](state::Finished const&) { return kStringFinished; },
                     [](state::Aborted const&) { return kStringAborted; },
                 },
                 state);
}

Snapshot::GuardedData::GuardedData(
    std::unique_ptr<IDatabaseSnapshot> databaseSnapshot,
    std::vector<std::shared_ptr<LogicalCollection>> shards)
    : databaseSnapshot(std::move(databaseSnapshot)) {
  for (auto&& shard : shards) {
    auto maybeShardId = ShardID::shardIdFromString(shard->name());
    ADB_PROD_ASSERT(maybeShardId.ok())
        << "Tried to create a Snapshot on Database Server for a collection "
           "that is not a shard "
        << shard->name();
    statistics.shards.emplace(maybeShardId.get(),
                              SnapshotStatistics::ShardStatistics{});
    this->shards.emplace_back(std::move(shard), nullptr);
  }
}

Snapshot::Snapshot(SnapshotId id, GlobalLogIdentifier gid,
                   std::vector<std::shared_ptr<LogicalCollection>> shards,
                   std::unique_ptr<IDatabaseSnapshot> databaseSnapshot,
                   LoggerContext loggerContext)
    : _id{id},
      _gid(std::move(gid)),
      _state(state::Ongoing{}),
      _guardedData(std::move(databaseSnapshot), std::move(shards)),
      loggerContext(loggerContext.with<logContextKeySnapshotId>(getId())) {
  LOG_CTX("d6c7f", DEBUG, this->loggerContext)
      << "Created snapshot with id " << _id;
}

auto Snapshot::fetch() -> ResultT<SnapshotBatch> {
  return _state.doUnderLock([&](auto& state) -> ResultT<SnapshotBatch> {
    return std::visit([&](auto& arg) { return generateBatch(arg); }, state);
  });
}

auto Snapshot::finish() -> Result {
  return _state.doUnderLock([&](auto& state) {
    return std::visit(
        overload{
            [&](state::Ongoing&) -> Result {
              auto isEmpty = _guardedData.doUnderLock(
                  [&](auto& data) { return data.shards.empty(); });
              if (!isEmpty) {
                LOG_CTX("23913", WARN, loggerContext)
                    << "Snapshot is being finished, but still has more "
                       "data!";
              }
              state = state::Finished{};
              LOG_CTX("9e190", DEBUG, loggerContext) << "Snapshot finished!";
              return {};
            },
            [&](state::Finished const&) -> Result {
              LOG_CTX("16d04", INFO, loggerContext)
                  << "Trying to finish snapshot, but it appears to be already "
                     "finished!";
              return {};
            },
            [&](state::Aborted const&) -> Result {
              LOG_CTX("83e35", WARN, loggerContext)
                  << "Trying to finish snapshot, but it appears to be "
                     "aborted!";
              return Result{
                  TRI_ERROR_INTERNAL,
                  fmt::format("Snapshot {} is already aborted!", getId())};
            },
        },
        state);
  });
}

void Snapshot::abort() {
  _state.doUnderLock([&](auto& state) {
    std::visit(
        overload{
            [&](state::Ongoing&) {
              auto isEmpty = _guardedData.doUnderLock(
                  [&](auto& data) { return data.shards.empty(); });
              if (!isEmpty) {
                LOG_CTX("5ce86", INFO, loggerContext)
                    << "Snapshot is being aborted, but still has more "
                       "data!";
              }
              state = state::Aborted{};
              LOG_CTX("a862c", DEBUG, loggerContext) << "Snapshot aborted!";
            },
            [&](state::Finished const&) {
              LOG_CTX("81ea0", INFO, loggerContext)
                  << "Trying to abort snapshot, but it appears to be already "
                     "finished!";
            },
            [&](state::Aborted const&) {
              LOG_CTX("4daf1", WARN, loggerContext)
                  << "Trying to abort snapshot, but it appears to be already "
                     "aborted!";
            },
        },
        state);
  });
}

[[nodiscard]] auto Snapshot::status() const -> SnapshotStatus {
  auto state = _state.doUnderLock([](auto& state) { return state; });
  auto statistics =
      _guardedData.doUnderLock([](auto& data) { return data.statistics; });
  return {state, std::move(statistics)};
}

auto Snapshot::getId() const -> SnapshotId { return _id; }

auto Snapshot::giveUpOnShard(ShardID const& shardId) -> Result {
  return _guardedData.doUnderLock([&](auto& data) -> Result {
    if (data.shards.empty()) {
      return {};
    }

    if (data.shards.back().first != nullptr &&
        data.shards.back().first->name() == shardId) {
      auto res = data.databaseSnapshot->resetTransaction();
      if (res.fail()) {
        LOG_CTX("38d54", ERR, loggerContext)
            << "Failed to reset snapshot transaction, this may prevent shard "
            << shardId << " from being dropped: " << res;
      }
      data.shards.pop_back();
      return res;
    }

    if (auto it = std::find_if(std::begin(data.shards), std::end(data.shards),
                               [&](auto const& shard) {
                                 return shard.first != nullptr &&
                                        shard.first->name() == shardId;
                               });
        it != std::end(data.shards)) {
      // Give up the shared pointer to the logical collection
      it->first.reset();
    }

    LOG_CTX("89271", DEBUG, loggerContext) << "Gave up on shard: " << shardId;
    return {};
  });
}

auto Snapshot::isInactive() const -> bool {
  return _state.doUnderLock([&](auto& state) {
    return std::holds_alternative<state::Finished>(state) ||
           std::holds_alternative<state::Aborted>(state);
  });
}

auto Snapshot::generateBatch(state::Ongoing const&) -> ResultT<SnapshotBatch> {
  LOG_CTX("f9226", DEBUG, loggerContext) << "Reading next batch";

  TRI_IF_FAILURE("DocumentStateSnapshot::infiniteSnapshot") {
    // Sleep before returning, so the follower doesn't go into a
    // busy loop.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // Keep the snapshot alive by returning empty batches.
    return SnapshotBatch{.snapshotId = getId(), .hasMore = true};
  }

  return _guardedData.doUnderLock([&](auto& data) -> ResultT<SnapshotBatch> {
    std::vector<ReplicatedOperation> operations;

    while (!data.shards.empty()) {
      auto& currentShard = data.shards.back();
      auto& shard = currentShard.first;
      auto& reader = currentShard.second;

      // The collection might have been dropped in the meantime.
      if (shard == nullptr || shard->deleted()) {
        data.shards.pop_back();
        LOG_CTX("c9fba", DEBUG, loggerContext)
            << "Skipping dropped shard " << shard->name();
        continue;
      }

      if (reader == nullptr) {
        // First time reading from this shard, taking its snapshot
        auto res = basics::catchToResultT([&]() {
          return data.databaseSnapshot->createCollectionReader(shard);
        });

        if (res.fail()) {
          if (res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
            data.shards.pop_back();
            LOG_CTX("3862c", DEBUG, loggerContext)
                << "Skipping dropped shard " << shard->name();
            continue;
          }

          LOG_CTX("5532c", ERR, loggerContext)
              << "Encountered unexpected error while creating collection "
                 "reader for shard "
              << shard->name() << ": " << res.result();
          return res.result();
        }

        reader = std::move(res.get());

        auto maybeShardID = ShardID::shardIdFromString(shard->name());
        ADB_PROD_ASSERT(maybeShardID.ok())
            << "Tried to replicate an operation for a collection that is not a "
               "shard "
            << shard->name();

        operations.reserve(3);
        {
          auto properties = VPackBuilder();
          try {
            properties = shard->toVelocyPackIgnore(
                {
                    StaticStrings::DataSourceId,
                    StaticStrings::DataSourceName,
                    StaticStrings::DataSourceGuid,
                    StaticStrings::ObjectId,
                },
                LogicalDataSource::Serialization::Inventory);
          } catch (basics::Exception const& exception) {
            LOG_CTX("b8f94", ERR, loggerContext)
                << "Failed to serialize shard " << shard->name()
                << " properties: " << exception.message();
            TRI_ASSERT(false)
                << exception.location() << ": [" << exception.code() << "] "
                << exception.message();
            throw;
          }
          operations.emplace_back(ReplicatedOperation::CreateShard{
              maybeShardID.get(), shard->type(),
              std::move(properties).sharedSlice()});

          LOG_CTX("c0864", DEBUG, loggerContext)
              << "Sending shard " << shard->name()
              << " over the wire along with its properties.";
        }

        data.statistics.shards.emplace(
            std::move(maybeShardID.get()),
            SnapshotStatistics::ShardStatistics{reader->getDocCount()});
      } else {
        operations.reserve(2);
      }

      break;
    }

    if (data.shards.empty()) {
      LOG_CTX("ca1cb", DEBUG, loggerContext)
          << "No more shards to read from. Returning empty "
             "batch.";
      return SnapshotBatch{.snapshotId = getId(), .hasMore = false};
    }

    auto& [shard, reader] = data.shards.back();

    VPackBuilder builder;
    TRI_IF_FAILURE("DocumentStateSnapshot::foreverReadingFromSameShard") {
      // Unless the shard is already empty, we'll keep the transaction ongoing.
      reader->read(builder, 0);
    }
    else {
      reader->read(builder, kBatchSizeLimit);
    }

    auto payload = std::move(builder).sharedSlice();
    auto payloadLen = payload.slice().length();
    auto payloadSize = payload.byteSize();
    auto maybeShardID = ShardID::shardIdFromString(shard->name());
    ADB_PROD_ASSERT(maybeShardID.ok()) << "Tried to replicate an operation for "
                                          "a collection that is not a shard "
                                       << shard->name();

    auto tid = TransactionId::createFollower();
    // During SnapshotTransfer, we do not want to account the operation to a
    // specific user, so we set an empty userName.
    operations.emplace_back(ReplicatedOperation::Insert{
        tid, maybeShardID.get(), std::move(payload), StaticStrings::Empty});
    operations.emplace_back(ReplicatedOperation::buildCommitOperation(tid));

    auto readerHasMore = reader->hasMore();
    if (!readerHasMore) {
      // Removing the shard from the list will decrease the reference count on
      // its logical collection
      data.shards.pop_back();
      // Resetting the transaction will allow the maintenance to drop or modify
      // the shard if needed
      data.databaseSnapshot->resetTransaction();

      LOG_CTX("c00b1", DEBUG, loggerContext)
          << "Reading from shard " << maybeShardID.get() << " completed. "
          << data.shards.size() << " shards to go.";
    }

    ++data.statistics.batchesSent;
    data.statistics.bytesSent += payloadSize;

    TRI_ASSERT(data.statistics.shards.contains(maybeShardID.get()))
        << getId() << " " << maybeShardID.get();
    data.statistics.shards[maybeShardID.get()].docsSent += payloadLen;

    data.statistics.lastBatchSent = data.statistics.lastUpdated =
        std::chrono::system_clock::now();

    LOG_CTX("9d1b4", DEBUG, loggerContext)
        << "Trx " << tid << " reading " << payloadLen << " documents from "
        << maybeShardID.get() << " in batch " << data.statistics.batchesSent
        << " with " << payloadSize << " bytes. There is "
        << (readerHasMore ? "" : "no") << " more data to read from this shard.";

    return SnapshotBatch{.snapshotId = getId(),
                         .hasMore = readerHasMore || !data.shards.empty(),
                         .operations = std::move(operations)};
  });
}

auto Snapshot::generateBatch(state::Finished const&) -> ResultT<SnapshotBatch> {
  LOG_CTX("fe02b", DEBUG, loggerContext)
      << "Trying to fetch data from a finished snapshot!";
  return ResultT<SnapshotBatch>::error(
      TRI_ERROR_INTERNAL,
      fmt::format("Snapshot {} is already finished!", getId()));
}

auto Snapshot::generateBatch(state::Aborted const&) -> ResultT<SnapshotBatch> {
  LOG_CTX("d4253", DEBUG, loggerContext)
      << "Trying to fetch data from an aborted snapshot!";
  return ResultT<SnapshotBatch>::error(
      TRI_ERROR_INTERNAL,
      fmt::format("Snapshot {} is already aborted!", getId()));
}

auto Snapshot::generateDocumentBatch(ShardID shardId,
                                     velocypack::SharedSlice slice)
    -> std::vector<ReplicatedOperation> {
  std::vector<ReplicatedOperation> batch;
  batch.reserve(2);
  auto tid = TransactionId::createFollower();
  // During SnapshotTransfer, we do not want to account the operation to a
  // specific user, so we set an empty userName.
  batch.emplace_back(
      ReplicatedOperation::Insert{tid, std::move(shardId), std::move(slice),
                                  StaticStrings::Empty, std::nullopt});
  batch.emplace_back(ReplicatedOperation::Commit{tid});
  return batch;
}
}  // namespace arangodb::replication2::replicated_state::document
