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

#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"

#include "Basics/DownCast.h"
#include "Replication2/StateMachines/Document/MaintenanceActionExecutor.h"
#include "Indexes/Index.h"
#include "IResearch/IResearchDataStore.h"
#include "IResearch/IResearchRocksDBInvertedIndex.h"
#include "IResearch/IResearchRocksDBLink.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#ifdef USE_V8
#include "Transaction/V8Context.h"
#endif
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/vocbase.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/VocBaseLogManager.h"

namespace arangodb::replication2::replicated_state::document {

DocumentStateShardHandler::DocumentStateShardHandler(
    TRI_vocbase_t& vocbase, GlobalLogIdentifier gid,
    std::shared_ptr<IMaintenanceActionExecutor> maintenance)
    : _gid(std::move(gid)),
      _maintenance(std::move(maintenance)),
      _vocbase(vocbase) {}

auto DocumentStateShardHandler::ensureShard(
    ShardID const& shard, TRI_col_type_e collectionType,
    velocypack::SharedSlice const& properties) noexcept -> Result {
  auto res =
      _maintenance->executeCreateCollection(shard, collectionType, properties);
  std::ignore = _maintenance->addDirty();
  return res;
}

auto DocumentStateShardHandler::modifyShard(
    ShardID shard, CollectionID colId,
    velocypack::SharedSlice properties) noexcept -> Result {
  auto col = lookupShard(shard);
  if (col.fail()) {
    return {col.errorNumber(),
            fmt::format("Error while modifying shard: {}", col.errorMessage())};
  }

  auto res = _maintenance->executeModifyCollection(
      std::move(col).get(), std::move(colId), std::move(properties));
  std::ignore = _maintenance->addDirty();

  return res;
}

auto DocumentStateShardHandler::dropShard(ShardID const& shard) noexcept
    -> Result {
  auto col = lookupShard(shard);
  if (col.fail()) {
    return {col.errorNumber(),
            fmt::format("Error while dropping shard: {}", col.errorMessage())};
  }

  auto res = _maintenance->executeDropCollection(std::move(*col));
  std::ignore = _maintenance->addDirty();

  return res;
}

auto DocumentStateShardHandler::dropAllShards() noexcept -> Result {
  auto collections =
      basics::catchToResultT([&]() { return getAvailableShards(); });
  if (collections.fail()) {
    return {collections.errorNumber(),
            fmt::format("Replicated log {} failed to get available shards: {}",
                        _gid, collections.errorMessage())};
  }

  std::stringstream err;
  auto res = Result{};
  for (auto&& col : *collections) {
    if (auto r = _maintenance->executeDropCollection(col); r.fail()) {
      err << col->name() << ": " << r << ", ";
      res.reset(std::move(r));
    }
  }

  std::ignore = _maintenance->addDirty();

  if (res.fail()) {
    res.reset(
        res.errorNumber(),
        fmt::format("Replicated log {} failed to drop shards: {} (error: {})",
                    _gid, err.str(), res.errorMessage()));
  }
  return res;
}

auto DocumentStateShardHandler::getAvailableShards()
    -> std::vector<std::shared_ptr<LogicalCollection>> {
  auto collections = _vocbase.collections(false);
  std::erase_if(collections, [&](auto&& col) {
    return col->replicatedStateId() != _gid.id;
  });
  return collections;
}

auto DocumentStateShardHandler::ensureIndex(
    ShardID shard, velocypack::Slice properties,
    std::shared_ptr<methods::Indexes::ProgressTracker> progress,
    Replication2Callback callback) noexcept -> Result {
  auto col = lookupShard(shard);
  if (col.fail()) {
    return {col.errorNumber(),
            fmt::format("Error while ensuring index: {}", col.errorMessage())};
  }

  auto res = _maintenance->executeCreateIndex(std::move(col).get(), properties,
                                              std::move(progress),
                                              std::move(callback));
  std::ignore = _maintenance->addDirty();

  if (res.fail()) {
    res.reset(
        res.errorNumber(),
        fmt::format(
            "Error: {}! Replicated log {} failed to ensure index on shard {}! "
            "Index: {}",
            res.errorMessage(), _gid, shard, properties.toJson()));
  }
  return res;
}

auto DocumentStateShardHandler::dropIndex(ShardID shard,
                                          IndexId indexId) noexcept -> Result {
  auto col = lookupShard(shard);
  if (col.fail()) {
    return {col.errorNumber(),
            fmt::format("Error while dropping index: {}", col.errorMessage())};
  }

  auto res = _maintenance->executeDropIndex(std::move(col).get(), indexId);
  std::ignore = _maintenance->addDirty();

  if (res.fail()) {
    res = Result{res.errorNumber(),
                 fmt::format("Error: {}! Replicated log {} failed to drop "
                             "index on shard {}! Index: {}",
                             res.errorMessage(), _gid, shard, indexId.id())};
  }
  return res;
}

auto DocumentStateShardHandler::lookupShard(ShardID const& shard) noexcept
    -> ResultT<std::shared_ptr<LogicalCollection>> {
  auto col = _vocbase.lookupCollection(std::string{shard});
  if (col == nullptr) {
    return Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  fmt::format("Replicated log {} failed to lookup shard {}",
                              _gid, shard)};
  }
  return col;
}

auto DocumentStateShardHandler::lockShard(ShardID const& shard,
                                          AccessMode::Type accessType,
                                          transaction::OperationOrigin origin)
    -> ResultT<std::unique_ptr<transaction::Methods>> {
  auto col = lookupShard(shard);
  if (col.fail()) {
    return Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  fmt::format("Failed to lookup shard {} in database {} while "
                              "locking it for operation {}",
                              shard, _vocbase.name(), origin.description)};
  }

#ifdef USE_V8
  auto ctx =
      transaction::V8Context::createWhenRequired(_vocbase, origin, false);
#else
  auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
#endif

  // Not replicating this transaction
  transaction::Options options;
  options.requiresReplication = false;

  auto trx = std::make_unique<SingleCollectionTransaction>(
      std::move(ctx), *col.get(), accessType, options);
  Result res = trx->begin();
  if (res.fail()) {
    return Result{res.errorNumber(),
                  fmt::format("Failed to lock shard {} in database "
                              "{} for operation {}. Error: {}",
                              shard, _vocbase.name(), origin.description,
                              res.errorMessage())};
  }
  return trx;
}

auto DocumentStateShardHandler::prepareShardsForLogReplay() noexcept -> void {
  for (auto const& shard : getAvailableShards()) {
    // The inverted indexes cannot work with duplicate LocalDocumentIDs within
    // the same commit interval. They however can if we have a commit in between
    // the two. If we replay one log we know there can never be a duplicate
    // LocalDocumentID.
    for (auto const& index : shard->getPhysical()->getReadyIndexes()) {
      if (index->type() == Index::TRI_IDX_TYPE_INVERTED_INDEX) {
        auto& idx =
            basics::downCast<iresearch::IResearchRocksDBInvertedIndex>(*index);
        TRI_ASSERT(!idx._isCreation) << "Inverted index still in creation mode";
        auto res = idx.commit(true);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        TRI_ASSERT(res.ok()) << "Failed to do first Inverted index commit";
        res = idx.commit(true);
        TRI_ASSERT(res.ok()) << "Failed to do second Inverted index commit";
        TRI_ASSERT(res.get() ==
                   iresearch::IResearchDataStore::CommitResult::NO_CHANGES)
            << "Inverted index still has changes after first commit.";
#endif
      } else if (index->type() == Index::TRI_IDX_TYPE_IRESEARCH_LINK) {
        auto& idx = basics::downCast<iresearch::IResearchRocksDBLink>(*index);
        TRI_ASSERT(!idx._isCreation)
            << "Search link index still in creation mode";
        auto res = idx.commit(true);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        TRI_ASSERT(res.ok()) << "Failed to do first Link index commit";
        res = idx.commit(true);
        TRI_ASSERT(res.ok()) << "Failed to do second Link index commit";
        TRI_ASSERT(res.get() ==
                   iresearch::IResearchDataStore::CommitResult::NO_CHANGES)
            << "Link index still has changes after first commit.";
#endif
      }
    }
  }
}

}  // namespace arangodb::replication2::replicated_state::document
