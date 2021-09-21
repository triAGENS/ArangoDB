////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Agency/AgencyComm.h"
#include "Aql/FixedVarExpressionContext.h"
#include "Aql/types.h"
#include "Basics/Common.h"
#include "Basics/FileUtils.h"
#include "Cluster/ClusterFeature.h"
#include "Futures/Future.h"
#include "Network/types.h"
#include "Rest/CommonDefines.h"
#include "Rest/GeneralResponse.h"
#include "Transaction/MethodsApi.h"
#include "Utils/OperationResult.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/voc-types.h"

#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

#include <map>

namespace arangodb {

namespace graph {
class ClusterTraverserCache;
}

namespace velocypack {
class Builder;
class HashedStringRef;
}  // namespace velocypack

namespace traverser {
struct TraverserOptions;
}

class ClusterFeature;
struct OperationOptions;

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a list of attributes have the same values in two vpack
/// documents
////////////////////////////////////////////////////////////////////////////////

bool shardKeysChanged(LogicalCollection const& collection, VPackSlice const& oldValue,
                      VPackSlice const& newValue, bool isPatch);

/// @brief check if the value of the smartJoinAttribute has changed
bool smartJoinAttributeChanged(LogicalCollection const& collection, VPackSlice const& oldValue,
                               VPackSlice const& newValue, bool isPatch);

/// @brief aggregate the results of multiple figures responses (e.g. from
/// multiple shards or for a smart edge collection)
void aggregateClusterFigures(bool details, bool isSmartEdgeCollectionPart,
                             arangodb::velocypack::Slice value,
                             arangodb::velocypack::Builder& builder);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns revision for a sharded collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> revisionOnCoordinator(ClusterFeature&,
                                                       std::string const& dbname,
                                                       std::string const& collname,
                                                       OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns checksum for a sharded collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> checksumOnCoordinator(
    ClusterFeature& feature, std::string const& dbname, std::string const& collname,
    OperationOptions const& options, bool withRevisions, bool withData);

////////////////////////////////////////////////////////////////////////////////
/// @brief Warmup index caches on Shards
////////////////////////////////////////////////////////////////////////////////

futures::Future<Result> warmupOnCoordinator(ClusterFeature&, std::string const& dbname,
                                            std::string const& cid,
                                            OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns figures for a sharded collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> figuresOnCoordinator(ClusterFeature&,
                                                      std::string const& dbname,
                                                      std::string const& collname, bool details,
                                                      OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief counts number of documents in a coordinator, by shard
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> countOnCoordinator(transaction::Methods& trx,
                                                    std::string const& collname,
                                                    OperationOptions const& options,
                                                    arangodb::transaction::MethodsApi api);

////////////////////////////////////////////////////////////////////////////////
/// @brief gets the selectivity estimates from DBservers
////////////////////////////////////////////////////////////////////////////////

Result selectivityEstimatesOnCoordinator(ClusterFeature&, std::string const& dbname,
                                         std::string const& collname,
                                         std::unordered_map<std::string, double>& result,
                                         TransactionId tid = TransactionId::none());

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> createDocumentOnCoordinator(
    transaction::Methods const& trx, LogicalCollection&, VPackSlice const slice,
    OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> removeDocumentOnCoordinator(transaction::Methods& trx,
                                                             LogicalCollection&,
                                                             VPackSlice const slice,
                                                             OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> getDocumentOnCoordinator(transaction::Methods& trx,
                                                          LogicalCollection&, VPackSlice slice,
                                                          OperationOptions const& options,
                                                          transaction::MethodsApi api);

/// @brief fetch edges from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        All non-empty and non-cached results
///        of DBServers will be inserted in the
///        datalake. Slices used in the result
///        point to content inside of this lake
///        only and do not run out of scope unless
///        the lake is cleared.
///        TraversalVariant

Result fetchEdgesFromEngines(transaction::Methods& trx, graph::ClusterTraverserCache& travCache,
                             arangodb::aql::FixedVarExpressionContext const& opts,
                             arangodb::velocypack::StringRef vertexId, size_t depth,
                             std::vector<arangodb::velocypack::Slice>& result);

/// @brief fetch edges from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        All non-empty and non-cached results
///        of DBServers will be inserted in the
///        datalake. Slices used in the result
///        point to content inside of this lake
///        only and do not run out of scope unless
///        the lake is cleared.
///        ShortestPathVariant

Result fetchEdgesFromEngines(transaction::Methods& trx, graph::ClusterTraverserCache& travCache,
                             arangodb::velocypack::Slice vertexId, bool backward,
                             std::vector<arangodb::velocypack::Slice>& result,
                             size_t& read);

/// @brief fetch vertices from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        If any server responds with a document
///        it will be inserted into the result.
///        If no server responds with a document
///        a 'null' will be inserted into the result.

void fetchVerticesFromEngines(
    transaction::Methods& trx, graph::ClusterTraverserCache& travCache,
    std::unordered_set<arangodb::velocypack::HashedStringRef>& vertexId,
    std::unordered_map<arangodb::velocypack::HashedStringRef, arangodb::velocypack::Slice>& result,
    bool forShortestPath);

////////////////////////////////////////////////////////////////////////////////
/// @brief modify a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> modifyDocumentOnCoordinator(
    transaction::Methods& trx, LogicalCollection& coll,
    arangodb::velocypack::Slice const& slice, OperationOptions const& options, bool isPatch);

////////////////////////////////////////////////////////////////////////////////
/// @brief truncate a cluster collection on a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> truncateCollectionOnCoordinator(
    transaction::Methods& trx, std::string const& collname, OperationOptions const& options);

////////////////////////////////////////////////////////////////////////////////
/// @brief flush Wal on all DBservers
////////////////////////////////////////////////////////////////////////////////

ErrorCode flushWalOnAllDBServers(ClusterFeature&, bool waitForSync, bool waitForCollector);

/// @brief compact the database on all DB servers
Result compactOnAllDBServers(ClusterFeature&, bool changeLevel, bool compactBottomMostLevel);

//////////////////////////////////////////////////////////////////////////////
/// @brief create hotbackup on a coordinator
//////////////////////////////////////////////////////////////////////////////

enum HotBackupMode { CONSISTENT, DIRTY };

/**
 * @brief Create hot backup on coordinators
 * @param mode    Backup mode: consistent, dirty
 * @param timeout Wait for this attempt and bail out if not met
 */
arangodb::Result hotBackupCoordinator(ClusterFeature&, VPackSlice const payload,
                                      VPackBuilder& report);

/**
 * @brief Restore specific hot backup on coordinators
 * @param mode    Backup mode: consistent, dirty
 * @param timeout Wait for this attempt and bail out if not met
 */
arangodb::Result hotRestoreCoordinator(ClusterFeature&, VPackSlice const payload,
                                       VPackBuilder& report);

/**
 * @brief List all
 * @param mode    Backup mode: consistent, dirty
 * @param timeout Wait for this attempt and bail out if not met
 */
arangodb::Result listHotBackupsOnCoordinator(ClusterFeature&, VPackSlice const payload,
                                             VPackBuilder& report);

/**
 * @brief Delete specific hot backup
 * @param backupId  BackupId to delete
 */
arangodb::Result deleteHotBackupsOnCoordinator(ClusterFeature&, VPackSlice const payload,
                                               VPackBuilder& report);

#ifdef USE_ENTERPRISE
/**
 * @brief Trigger upload of specific hot backup
 * @param backupId  BackupId to delete
 */
arangodb::Result uploadBackupsOnCoordinator(ClusterFeature&, VPackSlice const payload,
                                            VPackBuilder& report);

/**
 * @brief Trigger download of specific hot backup
 * @param backupId  BackupId to delete
 */
arangodb::Result downloadBackupsOnCoordinator(ClusterFeature&, VPackSlice const payload,
                                              VPackBuilder& report);
#endif

/**
 * @brief match backup servers
 * @param  planDump   Dump of plan from backup
 * @param  dbServers  This cluster's db servers
 * @param  match      Matched db servers
 * @return            Operation's success
 */
arangodb::Result matchBackupServers(VPackSlice const planDump,
                                    std::vector<ServerID> const& dbServers,
                                    std::map<std::string, std::string>& match);

arangodb::Result matchBackupServersSlice(VPackSlice const planServers,
                                         std::vector<ServerID> const& dbServers,
                                         std::map<ServerID, ServerID>& match);

/**
 * @brief apply database server matches to plan
 * @param  plan     Plan from hot backup
 * @param  matches  Match backup's server ids to new server ids
 * @param  newPlan  Resulting new plan
 * @return          Operation's result
 */
arangodb::Result applyDBServerMatchesToPlan(VPackSlice const plan,
                                            std::map<ServerID, ServerID> const& matches,
                                            VPackBuilder& newPlan);

/// @brief get the engine stats from all DB servers
arangodb::Result getEngineStatsFromDBServers(ClusterFeature&, VPackBuilder& report);

class ClusterMethods {
 public:
  // wrapper Class for static functions.
  // Cannot be instanciated.
  ClusterMethods() = delete;
  ~ClusterMethods() = delete;

  // @brief Create many new collections on coordinator from a Array of VPack
  // parameter Note that this returns a vector of newly allocated objects
  static std::vector<std::shared_ptr<LogicalCollection>> createCollectionOnCoordinator(
      TRI_vocbase_t& vocbase, arangodb::velocypack::Slice parameters,
      bool ignoreDistributeShardsLikeErrors, bool waitForSyncReplication,
      bool enforceReplicationFactor, bool isNewDatabase,
      std::shared_ptr<LogicalCollection> const& colPtr);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief Enterprise Relevant code to filter out hidden collections
  ///        that should not be triggered directly by operations.
  ////////////////////////////////////////////////////////////////////////////////

  static bool filterHiddenCollections(LogicalCollection const& c);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief Enterprise Relevant code to filter out hidden collections
  ///        that should not be included in links
  ////////////////////////////////////////////////////////////////////////////////
  static bool includeHiddenCollectionInLink(std::string const& name);

  /// @brief removes smart name suffixes from collection names.
  /// @param possiblySmartName  collection name with possible smart suffixes.
  /// Will be modified inplace 
  static void realNameFromSmartName(std::string& possiblySmartName);

 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief Persist collection in Agency and trigger shard creation process
  ////////////////////////////////////////////////////////////////////////////////

  static std::vector<std::shared_ptr<LogicalCollection>> persistCollectionsInAgency(
      ClusterFeature&, std::vector<std::shared_ptr<LogicalCollection>>& col,
      bool ignoreDistributeShardsLikeErrors, bool waitForSyncReplication,
      bool enforceReplicationFactor, bool isNewDatabase,
      std::shared_ptr<LogicalCollection> const& colPtr);
};

}  // namespace arangodb

