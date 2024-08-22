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

#include "Cluster/ClusterTypes.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/LoggerContext.h"
#include "VocBase/Methods/Indexes.h"

struct TRI_vocbase_t;
namespace arangodb {
class MaintenanceFeature;
struct ShardID;
}  // namespace arangodb

namespace arangodb::replication2::replicated_state::document {

struct IMaintenanceActionExecutor {
  virtual ~IMaintenanceActionExecutor() = default;

  virtual auto executeCreateCollection(
      ShardID const& shard, TRI_col_type_e collectionType,
      velocypack::SharedSlice properties) noexcept -> Result = 0;

  virtual auto executeDropCollection(
      std::shared_ptr<LogicalCollection> col) noexcept -> Result = 0;

  virtual auto executeModifyCollection(
      std::shared_ptr<LogicalCollection> col, CollectionID colId,
      velocypack::SharedSlice properties) noexcept -> Result = 0;

  virtual auto executeCreateIndex(
      std::shared_ptr<LogicalCollection> col, velocypack::Slice properties,
      std::shared_ptr<methods::Indexes::ProgressTracker> progress,
      Replication2Callback callback) noexcept -> Result = 0;

  virtual auto executeDropIndex(std::shared_ptr<LogicalCollection> col,
                                IndexId indexId) noexcept -> Result = 0;

  virtual auto addDirty() noexcept -> Result = 0;
};

class MaintenanceActionExecutor : public IMaintenanceActionExecutor {
 public:
  MaintenanceActionExecutor(GlobalLogIdentifier _gid, ServerID server,
                            MaintenanceFeature& maintenanceFeature,
                            TRI_vocbase_t& vocbase,
                            LoggerContext const& loggerContext);

  auto executeCreateCollection(ShardID const& shard,
                               TRI_col_type_e collectionType,
                               velocypack::SharedSlice properties) noexcept
      -> Result override;

  auto executeDropCollection(std::shared_ptr<LogicalCollection> col) noexcept
      -> Result override;

  auto executeModifyCollection(std::shared_ptr<LogicalCollection> col,
                               CollectionID colId,
                               velocypack::SharedSlice properties) noexcept
      -> Result override;

  auto executeCreateIndex(
      std::shared_ptr<LogicalCollection> col, velocypack::Slice properties,
      std::shared_ptr<methods::Indexes::ProgressTracker> progress,
      Replication2Callback callback) noexcept -> Result override;

  auto executeDropIndex(std::shared_ptr<LogicalCollection> col,
                        IndexId indexId) noexcept -> Result override;

  auto addDirty() noexcept -> Result override;

 private:
  GlobalLogIdentifier _gid;
  MaintenanceFeature& _maintenanceFeature;
  ServerID _server;

  // The vocbase reference remains valid for the lifetime of the executor.
  // Replicated logs are stopped before the vocbase is marked as dropped.
  TRI_vocbase_t& _vocbase;

  LoggerContext const _loggerContext;
};
}  // namespace arangodb::replication2::replicated_state::document
