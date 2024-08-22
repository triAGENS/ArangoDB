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

#include "Basics/Guarded.h"
#include "Basics/UnshackledMutex.h"
#include "Cluster/ClusterTypes.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "VocBase/AccessMode.h"
#include "VocBase/Methods/Indexes.h"
#include "Transaction/OperationOrigin.h"

#include <shared_mutex>

struct TRI_vocbase_t;

namespace arangodb {
struct ShardID;
}

namespace arangodb::replication2::replicated_state::document {

struct IMaintenanceActionExecutor;

struct IDocumentStateShardHandler {
  virtual ~IDocumentStateShardHandler() = default;
  virtual auto ensureShard(ShardID const& shard, TRI_col_type_e collectionType,
                           velocypack::SharedSlice const& properties) noexcept
      -> Result = 0;

  virtual auto modifyShard(ShardID shard, CollectionID colId,
                           velocypack::SharedSlice properties) noexcept
      -> Result = 0;

  virtual auto dropShard(ShardID const& shard) noexcept -> Result = 0;

  virtual auto dropAllShards() noexcept -> Result = 0;

  virtual auto getAvailableShards()
      -> std::vector<std::shared_ptr<LogicalCollection>> = 0;

  virtual auto ensureIndex(
      ShardID shard, velocypack::Slice properties,
      std::shared_ptr<methods::Indexes::ProgressTracker> progress,
      Replication2Callback callback) noexcept -> Result = 0;

  virtual auto dropIndex(ShardID shard, IndexId indexId) -> Result = 0;

  virtual auto lookupShard(ShardID const& shard) noexcept
      -> ResultT<std::shared_ptr<LogicalCollection>> = 0;

  virtual auto lockShard(ShardID const& shard, AccessMode::Type accessType,
                         transaction::OperationOrigin origin)
      -> ResultT<std::unique_ptr<transaction::Methods>> = 0;

  virtual auto prepareShardsForLogReplay() noexcept -> void = 0;
};

class DocumentStateShardHandler : public IDocumentStateShardHandler {
 public:
  explicit DocumentStateShardHandler(
      TRI_vocbase_t& vocbase, GlobalLogIdentifier gid,
      std::shared_ptr<IMaintenanceActionExecutor> maintenance);

  auto ensureShard(ShardID const& shard, TRI_col_type_e collectionType,
                   velocypack::SharedSlice const& properties) noexcept
      -> Result override;

  auto modifyShard(ShardID shard, CollectionID colId,
                   velocypack::SharedSlice properties) noexcept
      -> Result override;

  auto dropShard(ShardID const& shard) noexcept -> Result override;

  auto dropAllShards() noexcept -> Result override;

  auto getAvailableShards()
      -> std::vector<std::shared_ptr<LogicalCollection>> override;

  auto ensureIndex(ShardID shard, velocypack::Slice properties,
                   std::shared_ptr<methods::Indexes::ProgressTracker> progress,
                   Replication2Callback callback) noexcept -> Result override;

  auto dropIndex(ShardID shard, IndexId indexId) noexcept -> Result override;

  auto lookupShard(ShardID const& shard) noexcept
      -> ResultT<std::shared_ptr<LogicalCollection>> override;

  auto lockShard(ShardID const& shard, AccessMode::Type accessType,
                 transaction::OperationOrigin origin)
      -> ResultT<std::unique_ptr<transaction::Methods>> override;

  auto prepareShardsForLogReplay() noexcept -> void override;

 private:
  GlobalLogIdentifier _gid;
  std::shared_ptr<IMaintenanceActionExecutor> _maintenance;
  TRI_vocbase_t& _vocbase;
};

}  // namespace arangodb::replication2::replicated_state::document
