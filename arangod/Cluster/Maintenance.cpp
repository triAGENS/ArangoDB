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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "Maintenance.h"

#include "Agency/AgencyPaths.h"
#include "Agency/AgencyStrings.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/overload.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ResignShardLeadership.h"
#include "Indexes/Index.h"
#include "Inspection/VPack.h"
#include "IResearch/IResearchCommon.h"
#include "Logger/LogContextKeys.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/Counter.h"
#include "Metrics/Gauge.h"
#include "Metrics/Histogram.h"
#include "Metrics/LogScale.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedState/StateStatus.h"
#include "Replication2/Version.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Utils/DatabaseGuard.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Databases.h"
#include "VocBase/Validators.h"

#include <velocypack/Collection.h>
#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

#include <absl/strings/escaping.h>

#include <algorithm>
#include <array>

using namespace arangodb;
using namespace arangodb::consensus;
using namespace arangodb::basics;
using namespace arangodb::maintenance;
using namespace arangodb::methods;
using namespace arangodb::basics::StringUtils;

static std::unordered_set<std::string> const alwaysRemoveProperties({ID, NAME});
static std::unordered_set<std::string> const selectivityEstimates(
    {SELECTIVITY_ESTIMATE});
static VPackValue const VP_DELETE("delete");
static VPackValue const VP_SET("set");

static std::string_view const PRIMARY("primary");
static std::string_view const EDGE("edge");

namespace {

// Maps shardID to list of servers
using ShardMapView = std::unordered_map<ShardID, std::vector<std::string_view>>;

bool isViewIndex(std::string_view indexType) noexcept {
  return indexType == iresearch::StaticStrings::ViewArangoSearchType;
}

static int indexOf(VPackSlice const& slice, std::string const& val) {
  if (slice.isArray()) {
    int counter = 0;
    for (VPackSlice entry : VPackArrayIterator(slice)) {
      if (entry.isString() && entry.isEqualString(val)) {
        return counter;
      }
      counter++;
    }
  }
  return -1;
}

static std::shared_ptr<VPackBuilder> createProps(VPackSlice const& s) {
  TRI_ASSERT(s.isObject());
  return std::make_shared<VPackBuilder>(
      arangodb::velocypack::Collection::remove(s, alwaysRemoveProperties));
}

static std::shared_ptr<VPackBuilder> compareRelevantProps(
    VPackSlice const& first, VPackSlice const& second) {
  std::array<std::string_view, 7> const compareProperties{
      StaticStrings::WaitForSyncString,
      StaticStrings::Schema,
      StaticStrings::WriteConcern,
      StaticStrings::CacheEnabled,
      StaticStrings::InternalValidatorTypes,
      StaticStrings::ComputedValues,
      StaticStrings::GraphSmartGraphAttribute};
  auto result = std::make_shared<VPackBuilder>();
  {
    VPackObjectBuilder b(result.get());
    for (auto const& property : compareProperties) {
      auto const& planned = first.get(property);
      if (planned.isNone()) {
        continue;
      }
      bool isSame = true;
      // Register any change
      if (property == StaticStrings::Schema) {
        // special handling for schemas is required here, because the
        // format for schemas seems to have changed, and we need to
        // compare them in a more fuzzy way
        if (!ValidatorBase::isSame(planned, second.get(property))) {
          isSame = false;
        }
      } else if (property == StaticStrings::ComputedValues) {
        auto const isEmpty = [](VPackSlice slice) {
          // do to an oversight in the collection api it can happen that
          // having no computed values is encoded as `computedValues: []`.
          // This would trip a simple comparison. Instead, handle special cases.
          return slice.isNone() || slice.isNull() || slice.isEmptyArray();
        };

        if (!isEmpty(planned) || !isEmpty(second.get(property))) {
          if (!basics::VelocyPackHelper::equal(planned, second.get(property),
                                               false)) {
            isSame = false;
          }
        }
      } else if (!basics::VelocyPackHelper::equal(planned, second.get(property),
                                                  false)) {
        isSame = false;
      }

      if (!isSame) {
        result->add(property, planned);
      }
    }
  }
  return result;
}

static VPackBuilder compareIndexes(
    StorageEngine& engine, std::string const& dbname,
    std::string const& collname, std::string const& shname,
    VPackSlice const& plan, VPackSlice const& local,
    MaintenanceFeature::errors_t const& errors,
    containers::FlatHashSet<std::string>& indis) {
  TRI_ASSERT(plan.isArray());

  VPackBuilder builder;
  {
    VPackArrayBuilder a(&builder);
    for (auto const& pindex : VPackArrayIterator(plan)) {
      // Skip primary and edge indexes
      std::string_view ptype =
          pindex.get(StaticStrings::IndexType).stringView();
      if (ptype == PRIMARY || ptype == EDGE) {
        continue;
      }
      VPackSlice planId = pindex.get(ID);
      TRI_ASSERT(planId.isString());
      std::string planIdS = planId.copyString();
      std::string planIdWithColl = shname + "/" + planIdS;
      indis.emplace(planIdWithColl);

      // See, if we already have an index with the id given in the Plan:
      bool found = false;
      if (local.isArray()) {
        for (auto const& lindex : VPackArrayIterator(local)) {
          // Skip primary and edge indexes
          std::string_view ltype =
              lindex.get(StaticStrings::IndexType).stringView();
          if (ltype == PRIMARY || ltype == EDGE) {
            continue;
          }

          VPackSlice localId = lindex.get(ID);
          TRI_ASSERT(localId.isString());
          // The local ID has the form <collectionName>/<ID>, to compare,
          // we need to extract the local ID:
          std::string_view localIdS = localId.stringView();
          auto pos = localIdS.find('/');
          if (pos != std::string_view::npos) {
            localIdS = localIdS.substr(pos + 1);
          }

          if (localIdS == planIdS) {
            // Already have this id, so abort search:
            found = true;
            // We should be done now, this index already exists, and since
            // one cannot legally change the properties of an index, we
            // should be fine. However, for robustness sake, we compare,
            // if the local index found actually has the right properties,
            // if not, we schedule a dropIndex action:
            if (!arangodb::Index::Compare(engine, pindex, lindex, dbname)) {
              // To achieve this, we remove the long version of the ID
              // from the indis set. This way, the local index will be
              // dropped further down in handleLocalShard:
              indis.erase(planIdWithColl);
            }
            break;
          }
        }
      }
      if (!found) {
        // Finally check if we have an error for this index:
        bool haveError = false;
        std::string errorKey = dbname + "/" + collname + "/" + shname;
        auto it1 = errors.indexes.find(errorKey);
        if (it1 != errors.indexes.end()) {
          auto it2 = it1->second.find(planIdS);
          if (it2 != it1->second.end()) {
            // Verify that the error is for this particular index id:
            VPackSlice err(it2->second->data());
            VPackSlice idSlice = err.get(ID);
            if (idSlice.isString()) {
              std::string_view id = idSlice.stringView();
              if (id == planIdS) {
                haveError = true;
              }
            }
          }
        }
        if (!haveError) {
          builder.add(pindex);
        } else {
          LOG_TOPIC("ceb3d", DEBUG, Logger::MAINTENANCE)
              << "Previous failure exists for index " << planIdS << " on shard "
              << dbname << "/" << shname << " for central " << dbname << "/"
              << collname << "- skipping";
        }
      }
    }
  }

  return builder;
}

static std::string CreateLeaderString(std::string const& leaderId,
                                      bool shouldBeLeading) {
  if (shouldBeLeading) {
    return std::string();
  }
  TRI_ASSERT(!leaderId.empty());
  if (leaderId.front() == UNDERSCORE[0]) {
    return leaderId.substr(1);
  }
  return leaderId;
}

static bool isReplication2Leader(ShardID const& shname,
                                 ReplicatedLogStatusMap const& localLogs,
                                 ShardIdToLogIdMap const& localShardsToLogs) {
  auto it = localShardsToLogs.find(shname);
  // If the shard is not found, then it has not been created yet.
  if (it != std::end(localShardsToLogs)) {
    auto logId = it->second;
    auto logStatus = localLogs.find(logId);
    // NOTE: We would like to guarantee that the log exists, but we cannot
    // do that right now, we can eventually have the log removed by the
    // maintenance before the shard is dropped. Also note: The Log could be
    // stolen from the lookup map for shard and log deletion where there is a
    // small time-window, where the shard and log still exist, but are not
    // listed.
    if (logStatus != std::end(localLogs)) {
      return logStatus->second.status.role ==
             replication2::replicated_log::ParticipantRole::kLeader;
    }
  }
  return false;
}

static void handlePlanShard(
    StorageEngine& engine, uint64_t planIndex, VPackSlice const& cprops,
    VPackSlice const& ldb, std::string const& dbname,
    std::string const& colname, ShardID const& shname,
    std::string const& serverId, std::string const& leaderId,
    containers::FlatHashSet<ShardID>& commonShrds,
    containers::FlatHashSet<std::string>& indis,
    MaintenanceFeature::errors_t& errors,
    containers::FlatHashSet<DatabaseID>& makeDirty, bool& callNotify,
    std::vector<std::shared_ptr<ActionDescription>>& actions,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    replication::Version replicationVersion,
    std::optional<ReplicatedLogStatusMap> const& localLogs,
    std::optional<ShardIdToLogIdMap> const& shardsToLogs) {
  // First check if the shard is locked:
  auto it = shardActionMap.find(shname);
  if (it != shardActionMap.end()) {
    makeDirty.insert(dbname);
    // do not set callNotify here to avoid a busy loop
    LOG_TOPIC("aaed1", DEBUG, Logger::MAINTENANCE)
        << "Skipping handlePlanShard for shard " << shname
        << " because it is locked by an action: " << *it->second;
    return;
  }

  std::shared_ptr<ActionDescription> description;

  bool shouldBeLeading = serverId == leaderId;
  bool replication2Leader =
      replicationVersion == replication::Version::TWO &&
      isReplication2Leader(shname, localLogs.value(), shardsToLogs.value());

  commonShrds.emplace(shname);

  auto const lcol = ldb.get(std::string{shname});
  if (lcol.isObject()) {  // Have local collection with that name

    std::string_view const localLeader = lcol.get(THE_LEADER).stringView();
    bool leading = localLeader.empty();
    auto properties = compareRelevantProps(cprops, lcol);

    auto fullShardLabel = dbname + "/" + colname + "/" + shname;

    // Check if there is some in-sync-follower which is no longer in the Plan:
    std::string followersToDropString;
    if (leading && shouldBeLeading) {
      VPackSlice shards = cprops.get(SHARDS);
      if (shards.isObject()) {
        VPackSlice planServers = shards.get(std::string{shname});
        if (planServers.isArray()) {
          containers::FlatHashSet<std::string> followersToDrop;
          // Now we have two server lists (servers and
          // failoverCandidates, we are looking for a server which
          // occurs in either of them but not in the plan
          VPackSlice serverList = lcol.get(SERVERS);
          if (serverList.isArray()) {
            for (auto const& q : VPackArrayIterator(serverList)) {
              followersToDrop.insert(q.copyString());
            }
          }
          serverList = lcol.get(StaticStrings::FailoverCandidates);
          if (serverList.isArray()) {
            // And again for the failoverCandidates:
            for (auto const& q : VPackArrayIterator(serverList)) {
              followersToDrop.insert(q.copyString());
            }
          }
          // Remove those in Plan:
          for (auto const& p : VPackArrayIterator(planServers)) {
            if (p.isString()) {
              followersToDrop.erase(p.copyString());
            }
          }
          // Everything remaining in followersToDrop is something we
          // need to act on
          for (auto const& r : followersToDrop) {
            if (!followersToDropString.empty()) {
              followersToDropString.push_back(',');
            }
            followersToDropString += r;
          }
        }
      }
    }

    // If comparison has brought any updates
    TRI_ASSERT(properties->slice().isObject());
    if (properties->slice().length() > 0 || !followersToDropString.empty()) {
      if (errors.shards.find(fullShardLabel) == errors.shards.end()) {
        if (replicationVersion != replication::Version::TWO ||
            replication2Leader) {
          // Skip for replication 2 databases on followers
          description = std::make_shared<ActionDescription>(
              std::map<std::string, std::string>{
                  {NAME, UPDATE_COLLECTION},
                  {DATABASE, dbname},
                  {COLLECTION, colname},
                  {SHARD, shname},
                  {SERVER_ID, serverId},
                  {FOLLOWERS_TO_DROP, followersToDropString}},
              HIGHER_PRIORITY, true, std::move(properties));
          makeDirty.insert(dbname);
          callNotify = true;
          actions.emplace_back(std::move(description));
        }
      } else {
        LOG_TOPIC("0285b", DEBUG, Logger::MAINTENANCE)
            << "Previous failure exists for local shard " << dbname << "/"
            << shname << "for central " << dbname << "/" << colname
            << "- skipping";
      }
    }
    if (!leading && shouldBeLeading) {
      LOG_TOPIC("52412", DEBUG, Logger::MAINTENANCE)
          << "Triggering TakeoverShardLeadership job for shard " << dbname
          << "/" << colname << "/" << shname
          << ", local leader: " << lcol.get(THE_LEADER).stringView()
          << ", leader id: " << leaderId << ", my id: " << serverId
          << ", should be leader: ";
      description = std::make_shared<ActionDescription>(
          std::map<std::string, std::string>{
              {NAME, TAKEOVER_SHARD_LEADERSHIP},
              {DATABASE, dbname},
              {COLLECTION, colname},
              {SHARD, shname},
              {LOCAL_LEADER, std::string(localLeader)},
              {OLD_CURRENT_COUNTER, "0"},  // legacy, no longer used
              {PLAN_RAFT_INDEX, std::to_string(planIndex)}},
          LEADER_PRIORITY, true);
      makeDirty.insert(dbname);
      callNotify = true;
      actions.emplace_back(std::move(description));
    }

    // Indexes
    if (replicationVersion != replication::Version::TWO || replication2Leader) {
      // Skip for replication 2 databases on followers
      auto const& pindexes = cprops.get(INDEXES);
      if (pindexes.isArray()) {
        auto const& lindexes = lcol.get(INDEXES);
        auto difference = compareIndexes(engine, dbname, colname, shname,
                                         pindexes, lindexes, errors, indis);

        // Index errors are checked in `compareIndexes`. The loop below only
        // cares about those indexes that have no error.
        if (difference.slice().isArray()) {
          for (auto&& index : VPackArrayIterator(difference.slice())) {
            // Ensure index is exempt from locking for the shard, since we allow
            // these actions to run in parallel to others and to similar ones.
            // Note however, that new index jobs are intentionally not
            // discovered when the shard is locked for maintenance.
            auto indexTypeName =
                index.get(StaticStrings::IndexType).copyString();
            // For replication2 there is a race on modify link, the DBServer can
            // witness drop and create of the index at the same time. With this
            // flag we serialize ViewIndex drop, and create on the same shard.
            // All other indexes can still be created in parallel.
            bool doLockShard =
                replicationVersion == replication::Version::TWO &&
                isViewIndex(indexTypeName);
            makeDirty.insert(dbname);
            callNotify = true;
            actions.emplace_back(std::make_shared<ActionDescription>(
                std::map<std::string, std::string>{
                    {NAME, ENSURE_INDEX},
                    {DATABASE, dbname},
                    {COLLECTION, colname},
                    {SHARD, shname},
                    {StaticStrings::IndexType, std::move(indexTypeName)},
                    {FIELDS, index.get(FIELDS).toJson()},
                    {ID, index.get(ID).copyString()}},
                INDEX_PRIORITY, doLockShard,
                std::make_shared<VPackBuilder>(index)));
          }
        }
      }
    }
  } else {  // Create the collection, if not a previous error stops us
    if (!errors.shards.contains(dbname + "/" + colname + "/" + shname)) {
      if (replicationVersion != replication::Version::TWO || shouldBeLeading) {
        // Skip for replication 2 databases on followers
        // The collection has not been created yet, so we can't simply get the
        // replicated log from the cache. Our best bet is to follow along with
        // replication1's shouldBeLeading logic. Worst case, the action is going
        // to find out that there's not leader and do nothing.
        auto props = createProps(cprops);  // Only once might need often!
        description = std::make_shared<ActionDescription>(
            std::map<std::string, std::string>{
                {NAME, CREATE_COLLECTION},
                {COLLECTION, colname},
                {SHARD, shname},
                {DATABASE, dbname},
                {SERVER_ID, serverId},
                {THE_LEADER, CreateLeaderString(leaderId, shouldBeLeading)}},
            SLOW_OP_PRIORITY, true, std::move(props));
        makeDirty.insert(dbname);
        callNotify = true;
        actions.emplace_back(std::move(description));
      }
    } else {
      LOG_TOPIC("c1d8e", DEBUG, Logger::MAINTENANCE)
          << "Previous failure exists for creating local shard " << dbname
          << "/" << shname << " for central " << dbname << "/" << colname
          << " - skipping";
    }
  }
}

static void handleLocalShard(
    std::string const& dbname, ShardID const& shname, VPackSlice const& cprops,
    ShardMapView const& shardMap, containers::FlatHashSet<ShardID>& commonShrds,
    containers::FlatHashSet<std::string>& indis, std::string const& serverId,
    std::vector<std::shared_ptr<ActionDescription>>& actions,
    containers::FlatHashSet<DatabaseID>& makeDirty, bool& callNotify,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    replication::Version replicationVersion,
    std::optional<ReplicatedLogStatusMap> const& localLogs,
    std::optional<ShardIdToLogIdMap> const& shardsToLogs) {
  // First check if the shard is locked:
  auto iter = shardActionMap.find(shname);
  if (iter != shardActionMap.end()) {
    makeDirty.insert(dbname);
    // do not set callNotify here to avoid a busy loop
    LOG_TOPIC("aaed6", DEBUG, Logger::MAINTENANCE)
        << "Skipping handleLocalShard for shard " << shname
        << " because it is locked by an action: " << *iter->second;
    return;
  }

  std::shared_ptr<ActionDescription> description;

  auto const& it = commonShrds.find(shname);

  auto localLeader = cprops.get(THE_LEADER).stringView();
  bool const isLeading = localLeader.empty();
  bool const replication2Leader =
      replicationVersion == replication::Version::TWO &&
      isReplication2Leader(shname, localLogs.value(), shardsToLogs.value());
  if (it == commonShrds.end()) {
    if (replicationVersion != replication::Version::TWO || replication2Leader) {
      // This collection is not planned anymore, can drop it
      description = std::make_shared<ActionDescription>(
          std::map<std::string, std::string>{
              {NAME, DROP_COLLECTION}, {DATABASE, dbname}, {SHARD, shname}},
          isLeading ? LEADER_PRIORITY : FOLLOWER_PRIORITY, true);
      makeDirty.insert(dbname);
      callNotify = true;
      actions.emplace_back(std::move(description));
    }
    return;
  }
  // We dropped out before
  TRI_ASSERT(it != commonShrds.end());
  // The shard exists in both Plan and Local
  commonShrds.erase(it);  // it not a common shard?

  std::string_view plannedLeader;
  if (auto shMapIter = shardMap.find(shname); shMapIter != shardMap.end()) {
    plannedLeader = shMapIter->second[0];
  }

  bool const activeResign = isLeading && plannedLeader != serverId;
  bool const adjustResignState =
      (plannedLeader == UNDERSCORE + serverId &&
       localLeader != ResignShardLeadership::LeaderNotYetKnownString) ||
      (plannedLeader != serverId && localLeader == LEADER_NOT_YET_KNOWN);
  /*
   * We need to resign in the following cases:
   * 1) (activeResign) We think we are the leader locally,
   *    but the plan says we are not. (including, we are resigned)
   * 2) (adjustResignState) We are not leading, and not in resigned
   *     state, but the plan says we should be resigend.
   *    - This triggers on rebooted servers, that were in resign process
   *    - This triggers if the shard is moved from the server,
   *      before it actually took ownership.
   */

  if (replicationVersion != replication::Version::TWO &&
      (activeResign || adjustResignState)) {
    description = std::make_shared<ActionDescription>(
        std::map<std::string, std::string>{{NAME, RESIGN_SHARD_LEADERSHIP},
                                           {DATABASE, dbname},
                                           {SHARD, shname}},
        RESIGN_PRIORITY, true);
    makeDirty.insert(dbname);
    callNotify = true;
    actions.emplace_back(description);
  }

  // We only drop indexes, when collection is not being dropped already
  if (cprops.hasKey(INDEXES) &&
      (replicationVersion != replication::Version::TWO || replication2Leader)) {
    if (cprops.get(INDEXES).isArray()) {
      for (auto const& index : VPackArrayIterator(cprops.get(INDEXES))) {
        std::string_view type =
            index.get(StaticStrings::IndexType).stringView();
        if (type != PRIMARY && type != EDGE) {
          std::string const id = index.get(ID).copyString();

          // check if index is in plan
          if (indis.find(shname + "/" + id) != indis.end() ||
              indis.find(id) != indis.end()) {
            indis.erase(id);
          } else {
            // Note that drop index actions are exempt from locking, since we
            // want that they can run in parallel.
            // Exception are SearchIndexes, as they can conflict on attached
            // views during update.
            // For replication2 there is a race on modify link, the DBServer can
            // witness drop and create of the index at the same time. With this
            // flag we serialize ViewIndex drop, and create on the same shard.
            // All other indexes can still be dropped in parallel.
            bool doLockShard =
                replicationVersion == replication::Version::TWO &&
                isViewIndex(type);
            makeDirty.insert(dbname);
            callNotify = true;
            actions.emplace_back(std::make_shared<ActionDescription>(
                std::map<std::string, std::string>{{NAME, DROP_INDEX},
                                                   {DATABASE, dbname},
                                                   {SHARD, shname},
                                                   {"index", id}},
                INDEX_PRIORITY, doLockShard));
          }
        }
      }
    }
  }
}

/// @brief Get a map shardName -> servers
ShardMapView getShardMap(VPackSlice collections) {
  ShardMapView shardMap;
  {
    // Note: collections can be NoneSlice if database is already deleted.
    // But then shardMap can also be empty, so we are good.
    if (collections.isObject()) {
      shardMap.reserve(collections.length());
      for (auto collection : VPackObjectIterator(collections)) {
        TRI_ASSERT(collection.value.isObject());
        if (auto shards = collection.value.get(SHARDS); shards.isObject()) {
          for (auto shard : VPackObjectIterator(shards)) {
            std::vector<std::string_view> servers;
            servers.reserve(shard.value.length());
            for (auto srv : VPackArrayIterator(shard.value)) {
              servers.emplace_back(srv.stringView());
            }

            shardMap.emplace(ShardID(shard.key.stringView()),
                             std::move(servers));
          }
        }
      }
    }
  }
  return shardMap;
}
}  // namespace

void arangodb::maintenance::diffReplicatedLogs(
    DatabaseID const& database, ReplicatedLogStatusMap const& localLogs,
    ReplicatedLogSpecMap const& planLogs, std::string const& serverId,
    MaintenanceFeature::errors_t& errors,
    containers::FlatHashSet<DatabaseID>& makeDirty, bool& callNotify,
    std::vector<std::shared_ptr<ActionDescription>>& actions) {
  using namespace arangodb::replication2;
  auto const createReplicatedLogAction =
      [&](LogId id, agency::LogPlanSpecification const* spec) {
        auto specStr = std::invoke([&] {
          VPackBuilder builder;
          auto slice = VPackSlice::noneSlice();
          if (spec != nullptr) {
            velocypack::serialize(builder, *spec);
            slice = builder.slice();
          }
          return absl::Base64Escape(
              std::string_view{slice.startAs<char>(), slice.byteSize()});
        });
        auto description = std::make_shared<ActionDescription>(
            std::map<std::string, std::string>{
                {std::string(NAME), std::string(UPDATE_REPLICATED_LOG)},
                {std::string(DATABASE), database},
                {REPLICATED_LOG_ID, std::to_string(id.id())},
                {REPLICATED_LOG_SPEC, specStr},
            },
            NORMAL_PRIORITY, false);

        makeDirty.insert(database);
        callNotify = true;
        actions.emplace_back(std::move(description));
      };

  // check all plan log entries
  for (auto const& [logId, spec] : planLogs) {
    if (spec.currentTerm &&
        spec.participantsConfig.participants.contains(serverId)) {
      // check if there are logs that do not exist locally
      if (auto localIt = localLogs.find(spec.id);
          localIt == std::end(localLogs)) {
        createReplicatedLogAction(spec.id, &spec);
      } else {
        // check if the term is the same
        bool const requiresUpdate =
            std::invoke([&, &logStatus = localIt->second, &spec = spec] {
              auto const& [status, server] = logStatus;
              // check if term has changed
              auto currentTerm = status.getCurrentTerm();
              if (!currentTerm.has_value() ||
                  *currentTerm != spec.currentTerm->term) {
                return true;
              }
              auto rebootId = ServerState::instance()->getRebootId();
              if (rebootId != server.rebootId) {
                return true;
              }

              // check if participants generation has changed (in case we are
              // the leader)
              if (status.role == replicated_log::ParticipantRole::kLeader) {
                if (status.activeParticipantsConfig->generation <
                    spec.participantsConfig.generation) {
                  return true;
                }
              }
              return false;
            });

        // Create UpdateLogAction
        if (requiresUpdate) {
          createReplicatedLogAction(spec.id, &spec);
        }
      }
    }
  }

  for (auto const& [id, status] : localLogs) {
    bool const dropLog = std::invoke([&, &id = id] {
      // Drop a replicated log if
      // either it is no longer in plan or ...
      auto it = planLogs.find(id);
      if (it == std::end(planLogs)) {
        return true;
      }
      // ... we are no longer a participant
      auto const& spec = it->second;
      return !spec.currentTerm.has_value() ||
             (spec.participantsConfig.participants.find(serverId) ==
              spec.participantsConfig.participants.end());
    });

    if (dropLog) {
      createReplicatedLogAction(id, nullptr);
    }
  }
}

/// @brief calculate difference between plan and local for for databases
arangodb::Result arangodb::maintenance::diffPlanLocal(
    StorageEngine& engine,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    uint64_t planIndex,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& current,
    uint64_t currentIndex, containers::FlatHashSet<std::string> dirty,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    std::string const& serverId, MaintenanceFeature::errors_t& errors,
    containers::FlatHashSet<DatabaseID>& makeDirty, bool& callNotify,
    std::vector<std::shared_ptr<ActionDescription>>& actions,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    ReplicatedLogStatusMapByDatabase const& localLogsByDatabase,
    ShardIdToLogIdMapByDatabase const& localShardIdToLogId) {
  // You are entering the functional sector.
  // Vous entrez dans le secteur fonctionel.
  // Sie betreten den funktionalen Sektor.
  arangodb::Result result;
  containers::FlatHashSet<ShardID>
      commonShrds;  // Intersection collections plan&local
  containers::FlatHashSet<std::string>
      indis;  // Intersection indexes plan&local
  containers::FlatHashMap<std::string, replication::Version>
      replicationVersion;  // Replication version of databases

  // Plan to local mismatch ----------------------------------------------------
  // Create or modify if local databases are affected
  for (auto const& p : plan) {
    auto const& dbname = p.first;
    auto pb = p.second->slice()[0];
    auto const& pdb = pb.get(std::vector<std::string>{AgencyCommHelper::path(),
                                                      PLAN, DATABASES, dbname});

    if (!pdb.isObject()) {
      LOG_TOPIC("490dc", DEBUG, Logger::MAINTENANCE)
          << "Invalid object found in plan for database " << dbname
          << ", skipping";
      continue;
    }

    if (auto rv = pdb.get("replicationVersion"); !rv.isNone()) {
      auto version = replication::parseVersion(rv);
      TRI_ASSERT(version.ok());
      replicationVersion.emplace(dbname, version.get());
    } else {
      // If the "replicationVersion" field is missing this has to be an old
      // DB which defaults to version ONE. Note that while processing shards,
      // we'll take decisions based on the replication version.
      replicationVersion.emplace(dbname, replication::Version::ONE);
    }

    if (local.find(dbname) == local.end()) {
      if (errors.databases.find(dbname) == errors.databases.end()) {
        makeDirty.insert(dbname);
        callNotify = true;
        actions.emplace_back(std::make_shared<ActionDescription>(
            std::map<std::string, std::string>{
                {std::string(NAME), std::string(CREATE_DATABASE)},
                {std::string(DATABASE), std::move(dbname)}},
            HIGHER_PRIORITY, false, std::make_shared<VPackBuilder>(pdb)));
      } else {
        LOG_TOPIC("3a6a8", DEBUG, Logger::MAINTENANCE)
            << "Previous failure exists for creating database " << dbname
            << "skipping";
      }
    }
  }

  // Drop databases, which are no longer in plan ONLY DIRTY
  for (auto const& dbname : dirty) {
    if (local.find(dbname) != local.end()) {
      bool needDrop = false;
      auto it = plan.find(dbname);
      if (it == plan.end()) {
        needDrop = true;
      } else {
        VPackSlice pb = it->second->slice()[0];
        VPackSlice pdb = pb.get(std::vector<std::string>{
            AgencyCommHelper::path(), PLAN, DATABASES, dbname});
        if (pdb.isNone() || pdb.isEmptyObject()) {
          LOG_TOPIC("12274", INFO, Logger::MAINTENANCE)
              << "Dropping databases: pdb is "
              << std::string(pdb.isNone() ? "non Slice" : pdb.toJson());
          needDrop = true;
        }
      }
      if (needDrop) {
        makeDirty.insert(dbname);
        callNotify = true;
        actions.emplace_back(std::make_shared<ActionDescription>(
            std::map<std::string, std::string>{
                {std::string(NAME), std::string(DROP_DATABASE)},
                {std::string(DATABASE), dbname}},
            HIGHER_PRIORITY, false));
      }
    }
  }

  // Check errors for databases, which are no longer in plan and remove from
  // errors
  for (auto& database : errors.databases) {
    auto const& dbname = database.first;
    if (dirty.find(dbname) != dirty.end()) {
      if (plan.find(dbname) == plan.end()) {
        database.second.reset();
      }
    }
  }

  // Replicated Logs and States
  for (auto const& dbname : dirty) {
    using namespace arangodb::replication2;
    if (!plan.contains(dbname) || !localLogsByDatabase.contains(dbname)) {
      continue;
    }

    auto const collectLogInformation = [&] {
      auto const& localLogsInDatabase = localLogsByDatabase.at(dbname);
      auto planLogsInDatabase = ReplicatedLogSpecMap{};
      auto planLogInDatabaseSlice =
          plan.at(dbname)->slice()[0].get(cluster::paths::aliases::plan()
                                              ->replicatedLogs()
                                              ->database(dbname)
                                              ->vec());
      if (planLogInDatabaseSlice.isObject()) {
        for (auto [key, value] : VPackObjectIterator(planLogInDatabaseSlice)) {
          auto spec =
              velocypack::deserialize<agency::LogPlanSpecification>(value);
          planLogsInDatabase.emplace(spec.id, std::move(spec));
        }
      }

      return std::make_pair(std::ref(localLogsInDatabase),
                            std::move(planLogsInDatabase));
    };

    auto const& [localLogs, planLogs] = collectLogInformation();

    diffReplicatedLogs(dbname, localLogs, planLogs, serverId, errors, makeDirty,
                       callNotify, actions);
  }

  // Obtain a shardID -> LogId map (for replication2 only)
  auto getShardsToLogs =
      [&](DatabaseID const& dbname) -> std::optional<ShardIdToLogIdMap> {
    auto it = localShardIdToLogId.find(dbname);
    if (it != std::end(localShardIdToLogId)) {
      return it->second;
    }
    LOG_TOPIC("f9399", ERR, Logger::MAINTENANCE)
        << "Could not get the shard to log mapping of replication2 database: "
        << dbname;
    return std::nullopt;
  };

  // Obtain a local log status map (for replication2 only)
  auto getLocalLogsStatus =
      [&](DatabaseID const& dbname) -> std::optional<ReplicatedLogStatusMap> {
    auto it = localLogsByDatabase.find(dbname);
    if (it != std::end(localLogsByDatabase)) {
      return it->second;
    }
    LOG_TOPIC("701d8", ERR, Logger::MAINTENANCE)
        << "Could not get the local logs status of replication2 database: "
        << dbname;
    return std::nullopt;
  };

  // Obtain the replication version
  auto getReplicationVersion =
      [&](DatabaseID const& dbname) -> replication::Version {
    auto version = replicationVersion.find(dbname);
    TRI_ASSERT(version != replicationVersion.end()) << dbname;
    return version->second;
  };

  // Create or modify if local collections are affected
  for (auto const& dbname : dirty) {  // each dirty database
    auto const lit = local.find(dbname);
    auto const pit = plan.find(dbname);
    if (pit != plan.end() && lit != local.end()) {
      auto rv = getReplicationVersion(dbname);

      auto shardsToLogs = rv == replication::Version::TWO
                              ? getShardsToLogs(dbname)
                              : std::nullopt;
      if (rv == replication::Version::TWO && !shardsToLogs) {
        TRI_ASSERT(false) << "dbname: " << dbname
                          << "; map: " << localShardIdToLogId
                          << "; local: " << lit->second->toJson()
                          << "; plan: " << pit->second->toJson();
        continue;
      }

      auto localLogs = rv == replication::Version::TWO
                           ? getLocalLogsStatus(dbname)
                           : std::nullopt;
      if (rv == replication::Version::TWO && !localLogs) {
        TRI_ASSERT(false) << "dbname: " << dbname
                          << "; map: " << localShardIdToLogId
                          << "; local: " << lit->second->toJson()
                          << "; plan: " << pit->second->toJson();
        continue;
      }

      auto pdb = pit->second->slice()[0];
      std::vector<std::string> ppath{AgencyCommHelper::path(), PLAN,
                                     COLLECTIONS, dbname};
      if (!pdb.hasKey(ppath)) {
        continue;
      }
      pdb = pdb.get(ppath);
      try {
        auto const& ldb = lit->second->slice();
        if (ldb.isObject() && pdb.isObject()) {
          for (auto const& pcol :
               VPackObjectIterator(pdb, true)) {  // each plan collection
            auto const& cprops = pcol.value;
            // for each shard
            TRI_ASSERT(cprops.isObject());
            for (auto const& shard :
                 VPackObjectIterator(cprops.get(SHARDS))) {  // each shard

              if (shard.value.isArray()) {
                for (auto const& dbs : VPackArrayIterator(
                         shard.value)) {  // each dbserver with shard
                  // We only care for shards, where we find us as "serverId" or
                  // "_serverId"
                  if (dbs.isEqualString(serverId) ||
                      dbs.isEqualString(UNDERSCORE + serverId)) {
                    // at this point a shard is in plan, we have the db for it
                    auto maybeShardID =
                        ShardID::shardIdFromString(shard.key.stringView());
                    if (ADB_UNLIKELY(maybeShardID.fail())) {
                      TRI_ASSERT(false)
                          << "Malformed shard ID in agency: "
                          << shard.key.stringView() << " in db: " << dbname
                          << " and collection: " << pcol.key.stringView();
                      THROW_ARANGO_EXCEPTION(maybeShardID.result());
                    }
                    handlePlanShard(engine, planIndex, cprops, ldb, dbname,
                                    pcol.key.copyString(), maybeShardID.get(),
                                    serverId, shard.value[0].copyString(),
                                    commonShrds, indis, errors, makeDirty,
                                    callNotify, actions, shardActionMap, rv,
                                    localLogs, shardsToLogs);
                    break;
                  }
                }
              }  // else if (!shard.value.isArray()) - intentionally do nothing
            }
          }
        }
      } catch (std::exception const& e) {
        LOG_TOPIC("49e89", WARN, Logger::MAINTENANCE)
            << "Failed to get collection from local information: " << e.what();
      }
    }
  }

  // At this point commonShrds contains all shards that eventually reside on
  // this server, are in Plan and their database is present

  // Compare local to plan -----------------------------------------------------
  for (auto const& dbname : dirty) {  // each dirty database
    auto lit = local.find(dbname);
    if (lit == local.end()) {
      continue;
    }
    auto const& ldbname = lit->first;
    auto const ldbslice = lit->second->slice();  // local collection

    auto const pit = plan.find(ldbname);
    if (pit != plan.end()) {                    // have in plan
      auto plan = pit->second->slice()[0].get(  // plan collections
          std::vector<std::string>{AgencyCommHelper::path(), PLAN, COLLECTIONS,
                                   ldbname});
      if (ldbslice.isObject()) {
        // Note that if `plan` is not an object, then `getShardMap` will simply
        // return an empty object, which is fine for `handleLocalShard`, so we
        // do not have to check anything else here.

        auto rv = getReplicationVersion(dbname);

        auto shardsToLogs = rv == replication::Version::TWO
                                ? getShardsToLogs(dbname)
                                : std::nullopt;
        if (rv == replication::Version::TWO && !shardsToLogs) {
          TRI_ASSERT(false)
              << "dbname: " << dbname << "; map: " << localShardIdToLogId
              << "; local: " << lit->second->toJson()
              << "; plan: " << pit->second->toJson();
          continue;
        }

        auto localLogs = rv == replication::Version::TWO
                             ? getLocalLogsStatus(dbname)
                             : std::nullopt;
        if (rv == replication::Version::TWO && !localLogs) {
          TRI_ASSERT(false)
              << "dbname: " << dbname << "; map: " << localShardIdToLogId
              << "; local: " << lit->second->toJson()
              << "; plan: " << pit->second->toJson();
          continue;
        }

        // NOTE: shardMap only stays valid as long as plan is not deleted. Do
        // not return it.
        auto const shardMap = getShardMap(plan);  // plan shards -> servers
        for (auto const& lcol : VPackObjectIterator(ldbslice)) {
          auto maybeShardID = ShardID::shardIdFromString(lcol.key.stringView());
          if (ADB_UNLIKELY(maybeShardID.fail())) {
            TRI_ASSERT(false)
                << "Malformed shard ID in agency: " << lcol.key.stringView()
                << " in db: " << ldbname;
            THROW_ARANGO_EXCEPTION(maybeShardID.result());
          }
          handleLocalShard(ldbname, maybeShardID.get(), lcol.value, shardMap,
                           commonShrds, indis, serverId, actions, makeDirty,
                           callNotify, shardActionMap, rv, localLogs,
                           shardsToLogs);
        }
      }
    }
  }

  // See if shard errors can be thrown out:
  // Check all shard errors in feature, if database or collection gone -> reset
  // error

  for (auto& shard : errors.shards) {
    std::vector<std::string> path = split(shard.first, '/');
    auto const& dbname = path[0];
    auto const& colname = path[1];

    if (dirty.find(dbname) != dirty.end()) {  // only if among dirty
      auto const planit = plan.find(dbname);

      if (planit == plan.end() ||  // database gone
          !planit->second->slice()[0].hasKey(std::vector<std::string>{
              // collection gone
              AgencyCommHelper::path(), PLAN, COLLECTIONS, dbname, colname})) {
        shard.second.reset();
      }
    }
  }

  // See if index errors can be thrown out:
  // Check all shard errors in feature, if database, collection or index gone ->
  // reset error

  for (auto& shard : errors.indexes) {
    std::vector<std::string> path =
        split(shard.first, '/');  // dbname, collection, shardid
    auto const& dbname = path[0];
    auto const& colname = path[1];

    if (dirty.find(dbname) != dirty.end()) {  // only if among dirty

      auto const planit = plan.find(dbname);

      std::vector<std::string> path{AgencyCommHelper::path(), PLAN, COLLECTIONS,
                                    dbname, colname};
      if (planit == plan.end() ||                      // db gone
          !planit->second->slice()[0].hasKey(path)) {  // collection gone
        for (auto& index : shard.second) {
          index.second.reset();
        }
      } else {
        path.push_back(INDEXES);
        VPackSlice indexes = planit->second->slice()[0].get(path);
        TRI_ASSERT(indexes.isArray());
        if (indexes.isArray()) {
          for (auto& p : shard.second) {
            std::string const& id = p.first;
            bool found = false;
            for (auto const& ind : VPackArrayIterator(indexes)) {
              if (ind.get(ID).stringView() == id) {
                found = true;
                break;
              }
            }
            if (!found) {
              p.second.reset();
            }
          }
        }
      }
    }
  }

  // You are leaving the functional sector.
  // Vous sortez du secteur fonctionnel.
  // Sie verlassen den funktionalen Sektor.

  return result;
}

/// @brief handle plan for local databases

arangodb::Result arangodb::maintenance::executePlan(
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    uint64_t planIndex,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& current,
    uint64_t currentIndex, containers::FlatHashSet<std::string> const& dirty,
    containers::FlatHashSet<std::string> const& moreDirt,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    std::string const& serverId, arangodb::MaintenanceFeature& feature,
    VPackBuilder& report,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    ReplicatedLogStatusMapByDatabase const& localLogs,
    ShardIdToLogIdMapByDatabase const& localShardIdToLogId) {
  // Errors from maintenance feature
  MaintenanceFeature::errors_t errors;
  arangodb::Result result = feature.copyAllErrors(errors);
  if (!result.ok()) {
    LOG_TOPIC("9039d", ERR, Logger::MAINTENANCE)
        << "phaseOne: failed to acquire copy of errors from maintenance "
           "feature.";
    return result;
  }

  std::vector<std::shared_ptr<ActionDescription>> actions;
  // reserve a bit of memory up-front for some new actions
  actions.reserve(8);

  // build difference between plan and local
  report.add(VPackValue(AGENCY));
  {
    VPackArrayBuilder a(&report);
    containers::FlatHashSet<DatabaseID> makeDirty;
    bool callNotify = false;
    auto& engine =
        feature.server().getFeature<EngineSelectorFeature>().engine();
    diffPlanLocal(engine, plan, planIndex, current, currentIndex, dirty, local,
                  serverId, errors, makeDirty, callNotify, actions,
                  shardActionMap, localLogs, localShardIdToLogId);
    feature.addDirty(makeDirty, callNotify);
  }

  for (auto const& action : actions) {
    // check if any action from moreDirt
    // and db not in feature.dirty
    if (action->has(DATABASE) &&
        moreDirt.find(action->get(DATABASE)) != moreDirt.end() &&
        !feature.isDirty(action->get(DATABASE))) {
      LOG_TOPIC("38739", ERR, Logger::MAINTENANCE)
          << "Maintenance feature detected action " << *action
          << " for randomly chosen database";
      TRI_ASSERT(false);
    }
  }

  for (auto const& i : errors.databases) {
    if (i.second == nullptr) {
      feature.removeDBError(i.first);
    }
  }
  for (auto const& i : errors.shards) {
    if (i.second == nullptr) {
      feature.removeShardError(i.first);
    }
  }
  for (auto const& i : errors.indexes) {
    std::unordered_set<std::string> tmp;
    for (auto const& index : i.second) {
      if (index.second == nullptr) {
        tmp.emplace(index.first);
      }
    }
    if (!tmp.empty()) {
      feature.removeIndexErrors(i.first, tmp);
    }
  }

  bool const debugActions =
      arangodb::Logger::isEnabled(LogLevel::DEBUG, Logger::MAINTENANCE);

  if (debugActions) {
    // open ACTIONS
    TRI_ASSERT(report.isOpenObject());
    report.add(ACTIONS, VPackValue(VPackValueType::Array));
  }

  // enact all
  for (auto& action : actions) {
    LOG_TOPIC("8513c", DEBUG, Logger::MAINTENANCE)
        << "adding action " << action.get() << " to feature ";
    if (debugActions) {
      VPackObjectBuilder b(&report);
      action->toVelocyPack(report);
    }
    if (!action->isRunEvenIfDuplicate()) {
      feature.addAction(std::move(action), false);
    } else {
      TRI_ASSERT(action->has(SHARD));
      TRI_ASSERT(action->has(DATABASE));

      // Note: In theory this could throw, however we have transformed a shardID
      // to this string before so in praxis this is safe.
      ShardID shardName{action->get(SHARD)};

      bool ok = feature.lockShard(shardName, action);
      if (ok) {
        try {
          Result res = feature.addAction(std::move(action), false);
          if (res.fail()) {
            feature.unlockShard(shardName);
          }
        } catch (std::exception const& exc) {
          feature.unlockShard(shardName);
          LOG_TOPIC("86762", INFO, Logger::MAINTENANCE)
              << "Exception caught when adding action, unlocking shard "
              << shardName << " again: " << exc.what();
        }
      } else {
        TRI_ASSERT(action->has(DATABASE));
        std::string dbName = action->get(DATABASE);
        // For security measure let us flag this database as dirty.
        // This ensures we are going to recheck it next turn when the shard
        // is hopefully unlocked
        feature.addDirty(dbName);
      }
    }
  }
  if (debugActions) {
    // close ACTIONS
    report.close();
  }

  return result;
}

/// @brief add new database to current
void addDatabaseToTransactions(std::string const& name,
                               Transactions& transactions) {
  // [ {"dbPath":{}}, {"dbPath":{"oldEmpty":true}} ]

  std::string dbPath = CURRENT_COLLECTIONS + name;
  VPackBuilder operation;  // create database in current
  {
    VPackObjectBuilder b(&operation);
    operation.add(dbPath, VPackSlice::emptyObjectSlice());
  }
  VPackBuilder precondition;
  {
    VPackObjectBuilder b(&precondition);
    precondition.add(VPackValue(dbPath));
    {
      VPackObjectBuilder bb(&precondition);
      precondition.add("oldEmpty", VPackValue(true));
    }
  }
  transactions.push_back({operation, precondition});
}

/// @brief Phase one: Compare plan and local and create descriptions
arangodb::Result arangodb::maintenance::phaseOne(
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    uint64_t planIndex,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& current,
    uint64_t currentIndex, containers::FlatHashSet<std::string> const& dirty,
    containers::FlatHashSet<std::string> const& moreDirt,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    std::string const& serverId, MaintenanceFeature& feature,
    VPackBuilder& report,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    ReplicatedLogStatusMapByDatabase const& localLogs,
    ShardIdToLogIdMapByDatabase const& localShardIdToLogId) {
  auto start = std::chrono::steady_clock::now();

  arangodb::Result result;

  report.add(VPackValue(PHASE_ONE));
  {
    VPackObjectBuilder por(&report);

    // Execute database changes
    try {
      result = executePlan(plan, planIndex, current, currentIndex, dirty,
                           moreDirt, local, serverId, feature, report,
                           shardActionMap, localLogs, localShardIdToLogId);
    } catch (std::exception const& e) {
      LOG_TOPIC("55938", ERR, Logger::MAINTENANCE)
          << "Error executing plan: " << e.what();
    }
  }

  report.add(VPackValue(PLAN));
  {
    VPackObjectBuilder p(&report);
    report.add("Index", VPackValue(planIndex));
  }

  auto end = std::chrono::steady_clock::now();
  uint64_t total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();
  TRI_ASSERT(feature._phase1_runtime_msec != nullptr);
  feature._phase1_runtime_msec->count(total_ms);

  return result;
}

static VPackBuilder removeSelectivityEstimate(VPackSlice const& index) {
  TRI_ASSERT(index.isObject());
  return arangodb::velocypack::Collection::remove(index, selectivityEstimates);
}

static ResultT<std::vector<ServerID>> getLocalFollowers(
    DatabaseFeature& df, std::string const& database,
    std::string const& shard) {
  try {
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();
    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      auto res = Result{
          TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
          fmt::format(
              "Maintenance::getLocalFollowers: Failed to lookup collection {}",
              shard)};
      LOG_TOPIC("ce393", DEBUG, Logger::MAINTENANCE) << res;
      return res;
    }
    return collection->followers()->getCopy();
  } catch (std::exception const& e) {
    auto res = Result{
        TRI_ERROR_ARANGO_DATABASE_NOT_FOUND,
        fmt::format(
            "Maintenance::getLocalFollowers: Failed to lookup database {}, "
            "exception: {} (this is expected if the database was recently "
            "deleted).",
            database, e.what())};
    LOG_TOPIC("a4e35", WARN, Logger::MAINTENANCE) << res;
    return res;
  }
}

static std::tuple<VPackBuilder, bool, bool> assembleLocalCollectionInfo(
    DatabaseFeature& df, VPackSlice const& info,
    std::vector<std::string_view> const& planServers,
    std::string const& database, std::string const& shard,
    std::string const& ourselves, MaintenanceFeature::errors_t const& allErrors,
    replication::Version replicationVersion,
    ReplicatedLogStatusMapByDatabase const& localLogs) {
  VPackBuilder ret;

  try {
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();
    bool shardInSync;
    bool shardReplicated;

    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      std::string errorMsg(
          "Maintenance::assembleLocalCollectionInfo: Failed to lookup "
          "collection ");
      errorMsg += shard;
      LOG_TOPIC("33a3b", DEBUG, Logger::MAINTENANCE) << errorMsg;
      { VPackObjectBuilder o(&ret); }
      return {ret, true, true};
    }

    std::string errorKey = database + "/" +
                           std::to_string(collection->planId().id()) + "/" +
                           shard;
    {
      VPackObjectBuilder r(&ret);
      auto it = allErrors.shards.find(errorKey);
      if (it == allErrors.shards.end()) {
        ret.add(StaticStrings::Error, VPackValue(false));
        ret.add(StaticStrings::ErrorMessage, VPackValue(std::string()));
        ret.add(StaticStrings::ErrorNum, VPackValue(0));
      } else {
        VPackSlice errs(static_cast<uint8_t const*>(it->second->data()));
        ret.add(StaticStrings::Error, errs.get(StaticStrings::Error));
        ret.add(StaticStrings::ErrorNum, errs.get(StaticStrings::ErrorNum));
        ret.add(StaticStrings::ErrorMessage,
                errs.get(StaticStrings::ErrorMessage));
      }
      ret.add(VPackValue(INDEXES));
      {
        VPackArrayBuilder ixs(&ret);
        if (info.get(INDEXES).isArray()) {
          auto it1 = allErrors.indexes.find(errorKey);
          containers::FlatHashSet<std::string> indexesDone;
          // First the indexes as they are in Local, potentially replaced
          // by an error:
          for (auto const& index : VPackArrayIterator(info.get(INDEXES))) {
            std::string id = index.get(ID).copyString();
            indexesDone.insert(id);
            if (it1 != allErrors.indexes.end()) {
              auto it2 = it1->second.find(id);
              if (it2 != it1->second.end()) {
                // Add the error instead:
                ret.add(VPackSlice(
                    static_cast<uint8_t const*>(it2->second->data())));
                continue;
              }
            }
            ret.add(removeSelectivityEstimate(index).slice());
          }
          // Now all the errors for this shard, for which there is no index:
          if (it1 != allErrors.indexes.end()) {
            for (auto const& p : it1->second) {
              if (indexesDone.find(p.first) == indexesDone.end()) {
                ret.add(
                    VPackSlice(static_cast<uint8_t const*>(p.second->data())));
              }
            }
          }
        }
      }

      if (replicationVersion != replication::Version::TWO) {
        // Original replication 1 code
        size_t numFollowers;
        std::tie(numFollowers, std::ignore) =
            collection->followers()->injectFollowerInfo(ret);
        shardInSync = planServers.size() == numFollowers + 1;
        shardReplicated = numFollowers > 0;
      } else {
        auto const* status =
            [&]() -> replication2::maintenance::LogStatus const* {
          if (auto logs = localLogs.find(database); logs != localLogs.end()) {
            auto logId = collection->replicatedStateId();
            if (auto log = logs->second.find(logId);
                log != logs->second.end()) {
              if (log->second.status.role ==
                  arangodb::replication2::replicated_log::ParticipantRole::
                      kLeader) {
                return &log->second;
              }
            }
          }
          return nullptr;
        }();

        TRI_ASSERT(status != nullptr) << "Error while reporting to Current for "
                                         "database "
                                      << database << ", collection " << shard;

        if (status) {
          ret.add(VPackValue(maintenance::SERVERS));
          {
            VPackArrayBuilder ar(&ret);
            ret.add(VPackValue(ourselves));
            for (auto const& p : status->status.followersWithSnapshot) {
              ret.add(VPackValue(p));
            }
          }
          ret.add(VPackValue(StaticStrings::FailoverCandidates));
          {
            VPackArrayBuilder ar(&ret);
            ret.add(VPackValue(ourselves));
            for (auto const& p : status->status.followersWithSnapshot) {
              ret.add(VPackValue(p));
            }
          }
          shardInSync =
              status->status.followersWithSnapshot.size() + 1 ==
              status->status.activeParticipantsConfig->participants.size();
          shardReplicated = !status->status.followersWithSnapshot.empty();
        } else {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_REPLICATION_REPLICATED_LOG_NOT_THE_LEADER,
              fmt::format("Error while reporting to Current for database {}, "
                          "collection {}, because the replicated log could not "
                          "be used!",
                          database, shard));
        }
      }
    }
    return {ret, shardInSync, shardReplicated};
  } catch (std::exception const& e) {
    ret.clear();
    std::string errorMsg(
        "Maintenance::assembleLocalCollectionInfo: Failed to lookup "
        "database ");
    errorMsg += database;
    errorMsg += ", exception: ";
    errorMsg += e.what();
    errorMsg += " (this is expected if the database was recently deleted).";
    LOG_TOPIC("7fe5d", WARN, Logger::MAINTENANCE) << errorMsg;
    { VPackObjectBuilder o(&ret); }
    return {ret, true, true};
  }
}

bool equivalent(VPackSlice const& local, VPackSlice const& current) {
  TRI_ASSERT(local.isObject());
  TRI_ASSERT(current.isObject());
  for (auto const& i : VPackObjectIterator(local, true)) {
    if (!VPackNormalizedCompare::equals(i.value,
                                        current.get(i.key.stringView()))) {
      return false;
    }
  }
  return true;
}

static VPackBuilder assembleLocalDatabaseInfo(
    DatabaseFeature& df, std::string const& database,
    MaintenanceFeature::errors_t const& allErrors) {
  // This creates the VelocyPack that is put into
  // /Current/Databases/<dbname>/<serverID>  for a database.

  VPackBuilder ret;

  try {
    DatabaseGuard guard(df, database);
    auto vocbase = &guard.database();

    {
      VPackObjectBuilder o(&ret);
      auto it = allErrors.databases.find(database);
      if (it == allErrors.databases.end()) {
        ret.add(StaticStrings::Error, VPackValue(false));
        ret.add(StaticStrings::ErrorNum, VPackValue(0));
        ret.add(StaticStrings::ErrorMessage, VPackValue(""));
      } else {
        VPackSlice errs(static_cast<uint8_t const*>(it->second->data()));
        ret.add(StaticStrings::Error, errs.get(StaticStrings::Error));
        ret.add(StaticStrings::ErrorNum, errs.get(StaticStrings::ErrorNum));
        ret.add(StaticStrings::ErrorMessage,
                errs.get(StaticStrings::ErrorMessage));
      }
      ret.add(ID, VPackValue(std::to_string(vocbase->id())));
      ret.add("name", VPackValue(vocbase->name()));
    }

    return ret;
  } catch (std::exception const& e) {
    ret.clear();  // In case the above has mid air collision.
    std::string errorMsg(
        "Maintenance::assembleLocalDatabaseInfo: Failed to lookup database ");
    errorMsg += database;
    errorMsg += ", exception: ";
    errorMsg += e.what();
    LOG_TOPIC("989b6", DEBUG, Logger::MAINTENANCE) << errorMsg;
    { VPackObjectBuilder o(&ret); }
    return ret;
  }
}

static auto reportCurrentReplicatedLogLocal(
    replication2::replicated_log::QuickLogStatus const& status,
    RebootId const rebootId,
    replication2::agency::LogCurrentLocalState const* const currentLocal)
    -> std::optional<replication2::agency::LogCurrentLocalState> {
  // Check if there is term locally (i.e. in status)
  if (auto localTerm = status.getCurrentTerm(); localTerm.has_value()) {
    // If so, check if there is nothing in Agency/Current or the term value is
    // different. Also update snapshot information.
    const bool wantUpdate = [&] {
      // always report if no local present or term is different
      if (currentLocal == nullptr || currentLocal->term != *localTerm) {
        return true;
      }
      // report is state has changed
      if (currentLocal->state != status.localState) {
        return true;
      }
      // report is snapshot status has changed
      if (currentLocal->snapshotAvailable != status.snapshotAvailable) {
        return true;
      }
      return false;
    }();
    if (wantUpdate) {
      auto localStats = status.getLocalStatistics();
      TRI_ASSERT(
          localStats
              .has_value());  // if status has a term, then it has statistics
      replication2::agency::LogCurrentLocalState localState;
      localState.term = localTerm.value();
      localState.spearhead = localStats->spearHead;
      localState.snapshotAvailable = status.snapshotAvailable;
      localState.state = status.localState;
      localState.rebootId = rebootId;
      return localState;
    }
  }
  return std::nullopt;
}

static auto reportCurrentReplicatedLogLeader(
    replication2::replicated_log::QuickLogStatus const& status,
    ServerID const& serverId,
    replication2::agency::LogCurrent::Leader const* currentLeader)
    -> std::optional<replication2::agency::LogCurrent::Leader> {
  TRI_ASSERT(status.role ==
             replication2::replicated_log::ParticipantRole::kLeader)
      << "expected participant with leader role";

  bool const requiresUpdate = std::invoke([&] {
    // check if either there is no entry in current yet, the term has changed
    // or the participant config generation has changed or if leadership was
    // established in the meantime
    if (currentLeader == nullptr ||
        currentLeader->term != status.getCurrentTerm() ||
        currentLeader->leadershipEstablished != status.leadershipEstablished ||
        currentLeader->commitStatus != status.commitFailReason) {
      return true;
    }

    // check if the committed participants config needs an update
    if (status.committedParticipantsConfig != nullptr) {
      if (currentLeader->committedParticipantsConfig.has_value()) {
        return currentLeader->committedParticipantsConfig->generation !=
               status.committedParticipantsConfig->generation;
      }
      return true;
    }

    return false;
  });

  if (requiresUpdate) {
    std::optional<arangodb::replication2::agency::ParticipantsConfig>
        committedParticipantsConfig;
    if (status.committedParticipantsConfig != nullptr) {
      committedParticipantsConfig = *status.committedParticipantsConfig;
    }
    replication2::agency::LogCurrent::Leader leader;
    leader.term = *status.getCurrentTerm();
    leader.serverId = serverId;
    leader.leadershipEstablished = status.leadershipEstablished;
    leader.commitStatus = status.commitFailReason;
    leader.committedParticipantsConfig = std::move(committedParticipantsConfig);
    return leader;
  }

  return std::nullopt;
}

static void writeUpdateReplicatedLogLeader(
    VPackBuilder& report, replication2::LogId id, DatabaseID const& dbName,
    replication2::LogTerm localTerm,
    replication2::agency::LogCurrent::Leader const& leader) {
  // update Current/ReplicatedLogs/<dbname>/<logId>/leader/term with
  // currentTerm and precondition
  //  Plan/ReplicatedLogs/<dbname>/<logId>/term/term == currentTerm
  using namespace cluster::paths;
  auto reportPath = aliases::current()
                        ->replicatedLogs()
                        ->database(dbName)
                        ->log(to_string(id))
                        ->leader()
                        ->str(SkipComponents(
                            1) /* skip first path component, i.e. 'arango' */);
  auto preconditionPath =
      aliases::plan()
          ->replicatedLogs()
          ->database(dbName)
          ->log(to_string(id))
          ->currentTerm()
          ->term()
          ->str(
              SkipComponents(1) /* skip first path component, i.e. 'arango' */);
  report.add(VPackValue(reportPath));
  {
    VPackObjectBuilder o(&report);
    report.add(OP, VP_SET);
    report.add(VPackValue("payload"));
    velocypack::serialize(report, leader);
    {
      VPackObjectBuilder preconditionBuilder(&report, "preconditions");
      report.add(preconditionPath, VPackValue(localTerm));
    }
  }
}

static auto reportCurrentReplicatedLogSafeRebootIds(
    replication2::replicated_log::QuickLogStatus const& status,
    std::optional<replication2::agency::LogCurrent> const& current)
    -> std::optional<
        std::unordered_map<replication2::ParticipantId, RebootId>> {
  auto statusSafeRebootIds = status.safeRebootIds != nullptr
                                 ? std::optional{*status.safeRebootIds}
                                 : std::nullopt;
  if (!current.has_value() or current->safeRebootIds != statusSafeRebootIds) {
    // Note that this may still be std::nullopt, which means no update.
    return statusSafeRebootIds;
  }
  return std::nullopt;
}

static void writeUpdateReplicatedLogSafeRebootIds(
    VPackBuilder& report, replication2::LogId id, DatabaseID const& dbName,
    replication2::LogTerm& localTerm,
    std::unordered_map<replication2::ParticipantId, RebootId> const&
        safeRebootIds) {
  // update Current/ReplicatedLogs/<dbname>/<logId>/safeRebootIds
  // with precondition
  // Plan/ReplicatedLogs/<dbname>/<logId>/currentTerm/term == localTerm
  using namespace cluster::paths;
  auto reportPath = aliases::current()
                        ->replicatedLogs()
                        ->database(dbName)
                        ->log(to_string(id))
                        ->safeRebootIds()
                        ->str(SkipComponents(
                            1) /* skip first path component, i.e. 'arango' */);
  auto preconditionPath =
      aliases::plan()
          ->replicatedLogs()
          ->database(dbName)
          ->log(to_string(id))
          ->currentTerm()
          ->term()
          ->str(
              SkipComponents(1) /* skip first path component, i.e. 'arango' */);
  report.add(VPackValue(reportPath));
  {
    VPackObjectBuilder o(&report);
    report.add(OP, VP_SET);
    report.add(VPackValue("payload"));
    velocypack::serialize(report, safeRebootIds);
    {
      VPackObjectBuilder preconditionBuilder(&report, "preconditions");
      report.add(preconditionPath, VPackValue(localTerm));
    }
  }
}

static void writeUpdateReplicatedLogLocal(
    VPackBuilder& report, replication2::LogId id, DatabaseID const& dbName,
    ServerID const& serverId, replication2::LogTerm localTerm,
    replication2::agency::LogCurrentLocalState const& local) {
  // Check
  // Current/ReplicatedLogs/<dbname>/<logId>/localStatus/<serverId>/currentTerm
  // != currentTerm if so, update
  // Current/ReplicatedLogs/<dbname>/<logId>/localStatus/<serverId> with
  //  {"currentTerm": currentTerm, "spearHead": {"index": last-index, "term":
  //  last-term}}
  // and precondition
  //  Plan/ReplicatedLogs/<dbname>/<logId>/term/term == currentTerm

  using namespace cluster::paths;
  auto reportPath = aliases::current()
                        ->replicatedLogs()
                        ->database(dbName)
                        ->log(to_string(id))
                        ->localStatus()
                        ->participant(serverId)
                        ->str(SkipComponents(
                            1) /* skip first path component, i.e. 'arango' */);
  auto preconditionPath =
      aliases::plan()
          ->replicatedLogs()
          ->database(dbName)
          ->log(to_string(id))
          ->currentTerm()
          ->term()
          ->str(
              SkipComponents(1) /* skip first path component, i.e. 'arango' */);
  report.add(VPackValue(reportPath));
  {
    VPackObjectBuilder o(&report);
    report.add(OP, VP_SET);
    report.add(VPackValue("payload"));
    velocypack::serialize(report, local);
    {
      VPackObjectBuilder preconditionBuilder(&report, "preconditions");
      report.add(preconditionPath, VPackValue(localTerm));
    }
  }
}

static void reportCurrentReplicatedLog(
    VPackBuilder& report, replication2::maintenance::LogStatus const& logStatus,
    VPackSlice cur, replication2::LogId id, std::string const& dbName) {
  using namespace replication2::agency;
  auto const& status = logStatus.status;
  auto const& serverId = logStatus.server.serverId;
  auto logContext =
      LoggerContext{Logger::MAINTENANCE}.with<logContextKeyLogId>(id);
  auto localTerm = status.getCurrentTerm();
  LOG_CTX("11dbd", TRACE, logContext)
      << "checking replicated log " << id
      << " local term = " << (localTerm ? to_string(*localTerm) : "n/a");

  if (!localTerm.has_value()) {
    return;
  }
  if (!cur.isObject()) {
    return;
  }
  // load current into memory
  auto current = std::invoke([&]() -> std::optional<LogCurrent> {
    auto currentSlice = cur.get(cluster::paths::aliases::current()
                                    ->replicatedLogs()
                                    ->database(dbName)
                                    ->log(to_string(id))
                                    ->vec());
    if (currentSlice.isNone()) {
      return std::nullopt;
    }
    return velocypack::deserialize<LogCurrent>(currentSlice);
  });

  {
    auto localState = std::invoke([&]() -> LogCurrentLocalState const* {
      if (current.has_value()) {
        if (auto iter = current->localState.find(serverId);
            iter != std::end(current->localState)) {
          return &iter->second;
        }
      }
      return nullptr;
    });

    if (auto result = reportCurrentReplicatedLogLocal(
            status, logStatus.server.rebootId, localState);
        result.has_value()) {
      writeUpdateReplicatedLogLocal(report, id, dbName, serverId, *localTerm,
                                    *result);
    }
  }

  {
    if (status.role == replication2::replicated_log::ParticipantRole::kLeader) {
      auto currentLeader =
          std::invoke([&]() -> replication2::agency::LogCurrent::Leader const* {
            if (current.has_value()) {
              if (current->leader.has_value()) {
                return &current->leader.value();
              }
            }
            return nullptr;
          });
      if (auto result =
              reportCurrentReplicatedLogLeader(status, serverId, currentLeader);
          result.has_value()) {
        writeUpdateReplicatedLogLeader(report, id, dbName, *localTerm, *result);
      }
      if (auto result =
              reportCurrentReplicatedLogSafeRebootIds(status, current);
          result.has_value()) {
        writeUpdateReplicatedLogSafeRebootIds(report, id, dbName, *localTerm,
                                              *result);
      }
    }
  }
}

// updateCurrentForCollections
// diff current and local and prepare agency transactions or whatever
// to update current. Will report the errors created locally to the agency
arangodb::Result arangodb::maintenance::reportInCurrent(
    MaintenanceFeature& feature,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    containers::FlatHashSet<std::string> const& dirty,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& current,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    MaintenanceFeature::errors_t const& allErrors, std::string const& serverId,
    VPackBuilder& report, ShardStatistics& shardStats,
    ReplicatedLogStatusMapByDatabase const& localLogs,
    ShardIdToLogIdMapByDatabase const& localShardIdToLogId) {
  for (auto const& dbName : dirty) {
    auto lit = local.find(dbName);
    VPackSlice ldb;
    if (lit == local.end()) {
      LOG_TOPIC("324e7", TRACE, Logger::MAINTENANCE)
          << "database " << dbName << " missing in local";
    } else {
      ldb = lit->second->slice();
    }

    auto cit = current.find(dbName);
    VPackSlice cur;
    if (cit == current.end()) {
      LOG_TOPIC("427e3", TRACE, Logger::MAINTENANCE)
          << dbName << " missing in current";
    } else {
      TRI_ASSERT(cit->second->slice().isArray());
      TRI_ASSERT(cit->second->slice().length() == 1);
      cur = cit->second->slice()[0];
    }

    auto& df = feature.server().getFeature<DatabaseFeature>();
    auto localDBExists = false;
    auto replicationVersion = std::invoke([&]() {
      // Fetch the replication version locally, instead of using the one from
      // Plan. This is because the database might have been removed from Plan in
      // the meantime.
      auto res = basics::catchToResultT([&]() -> replication::Version {
        DatabaseGuard guard(df, dbName);
        localDBExists = true;
        return guard->replicationVersion();
      });
      if (res.ok()) {
        return res.get();
      }

      LOG_TOPIC("fbff0", DEBUG, Logger::MAINTENANCE)
          << "Could not get the replication version of database (most likely "
             "it is no longer available): "
          << dbName << ", assuming replication version 1; " << res.result();
      return replication::Version::ONE;
    });

    // NOTE: shardMap only stays valid as long as plan is not deleted. Do not
    // return it.
    ShardMapView shardMap;
    auto pit = plan.find(dbName);
    VPackSlice pdb;
    if (pit == plan.end()) {
      LOG_TOPIC("47e23", TRACE, Logger::MAINTENANCE)
          << dbName << " missing in plan";
    } else {
      TRI_ASSERT(pit->second->slice().isArray());
      TRI_ASSERT(pit->second->slice().length() == 1);
      pdb = pit->second->slice()[0];
      std::vector<std::string> ppath{AgencyCommHelper::path(), PLAN,
                                     COLLECTIONS, dbName};
      TRI_ASSERT(pdb.isObject());
      if (!localDBExists) {
        // If the local database is not found, the replication version is
        // assumed to be ONE. As fallback, the following code checks for the
        // replication version in Plan.
        std::vector<std::string> dbpath{AgencyCommHelper::path(), PLAN,
                                        DATABASES, dbName};
        if (auto db = pdb.get(dbpath); db.isObject()) {
          if (auto rv = db.get("replicationVersion"); rv.isString()) {
            auto result = replication::parseVersion(rv);
            TRI_ASSERT(result.ok()) << result.result();
            replicationVersion = result.get();
          }
        }
      }
      // Plan of this database's collections
      pdb = pdb.get(ppath);
      if (!pdb.isNone()) {
        shardMap = getShardMap(pdb);
      }
    }
    auto cdbpath = std::vector<std::string>{AgencyCommHelper::path(), CURRENT,
                                            DATABASES, dbName, serverId};
    if (ldb.isObject()) {
      if (cur.isNone() || (cur.isObject() && !cur.hasKey(cdbpath))) {
        auto const localDatabaseInfo =
            assembleLocalDatabaseInfo(df, dbName, allErrors);
        TRI_ASSERT(!localDatabaseInfo.slice().isNone());
        if (!localDatabaseInfo.slice().isEmptyObject() &&
            !localDatabaseInfo.slice().isNone()) {
          report.add(VPackValue(CURRENT_DATABASES + dbName + "/" + serverId));
          {
            VPackObjectBuilder o(&report);
            report.add(OP, VP_SET);
            report.add("payload", localDatabaseInfo.slice());
          }
        }
      }

      auto shardsToLogs = replicationVersion == replication::Version::TWO
                              ? localShardIdToLogId.find(dbName)
                              : std::end(localShardIdToLogId);
      if (replicationVersion == replication::Version::TWO &&
          shardsToLogs == std::end(localShardIdToLogId)) {
        LOG_TOPIC("4c3c9", ERR, Logger::MAINTENANCE)
            << "Could not get the shard to log mapping of replication2 "
               "database: "
            << dbName;
        TRI_ASSERT(false) << "dbname: " << dbName
                          << "; map: " << localShardIdToLogId
                          << "; local: " << lit->second->toJson()
                          << "; plan: " << pit->second->toJson();
        continue;
      }

      auto logs = replicationVersion == replication::Version::TWO
                      ? localLogs.find(dbName)
                      : std::end(localLogs);
      if (replicationVersion == replication::Version::TWO &&
          logs == std::end(localLogs)) {
        LOG_TOPIC("04470", ERR, Logger::MAINTENANCE)
            << "Could not get the local logs status of replication2 database: "
            << dbName;
        TRI_ASSERT(false) << "dbname: " << dbName
                          << "; map: " << localShardIdToLogId
                          << "; local: " << lit->second->toJson()
                          << "; plan: " << pit->second->toJson();
        continue;
      }

      for (auto const& shard : VPackObjectIterator(ldb, true)) {
        auto const shName = ShardID{shard.key.stringView()};
        auto const shSlice = shard.value;
        TRI_ASSERT(shSlice.isObject());
        auto const colName =
            shSlice.get(StaticStrings::DataSourcePlanId).copyString();
        shardStats.numShards += 1;

        if (replicationVersion == replication::Version::TWO &&
            !isReplication2Leader(shName, logs->second, shardsToLogs->second)) {
          // skip this shard if it is not the leader
          continue;
        }

        VPackBuilder error;
        if (shSlice.get(THE_LEADER).copyString().empty()) {  // Leader
          try {
            // Check that we are the leader of this shard in the Plan, together
            // with the precondition below that the Plan is unchanged, this
            // ensures that we only ever modify Current if we are the leader in
            // the Plan:

            auto const planPath =
                std::vector<std::string>{colName, "shards", shName};
            if (!pdb.isObject() || !pdb.hasKey(planPath)) {
              LOG_TOPIC("43242", DEBUG, Logger::MAINTENANCE)
                  << "Ooops, we have a shard for which we believe to be the "
                     "leader, but the Plan does not have it any more, we do "
                     "not "
                     "report in Current about this, database: "
                  << dbName << ", shard: " << shName;
              continue;
            }

            TRI_ASSERT(pdb.isObject() && pdb.hasKey(planPath));

            // In the context of replication2, the leadership is attributed to a
            // replicated log, rather than a shard. Therefore, we must perform
            // similar leadership checks from the replicated log perspective.
            using CurrentTerm =
                std::pair<replication2::LogId, replication2::LogTerm>;
            auto fetchReplicatedLogTerm = [&]() -> std::optional<CurrentTerm> {
              // We have already checked that the local shard has a
              // corresponding replicated log and this server is the leader of
              // it. The assertions are here just as a double sanity check.
              auto logId = shardsToLogs->second.find(shName);
              TRI_ASSERT(logId != std::end(shardsToLogs->second));
              auto logStatus = logs->second.find(logId->second);
              TRI_ASSERT(logStatus != std::end(logs->second));
              auto currentTerm = logStatus->second.status.getCurrentTerm();
              if (currentTerm.has_value()) {
                return std::make_pair(logId->second, *currentTerm);
              }
              return std::nullopt;
            };

            // While the local replicated log believes to be the leader, there's
            // a slight chance that this is no longer the case in Plan. In order
            // for the corresponding list of severs to be updated under
            // `planPath`, another supervision round must be triggered. Hence,
            // we have to use a precondition in order to update Current for this
            // shard.
            std::optional<CurrentTerm> localTerm =
                replicationVersion == replication::Version::TWO
                    ? fetchReplicatedLogTerm()
                    : std::nullopt;

            VPackSlice thePlanList = pdb.get(planPath);
            if (!thePlanList.isArray() || thePlanList.length() == 0 ||
                !thePlanList[0].isString() ||
                !thePlanList[0].isEqualStringUnchecked(serverId) ||
                (replicationVersion == replication::Version::TWO &&
                 !localTerm.has_value())) {
              LOG_TOPIC("87776", DEBUG, Logger::MAINTENANCE)
                  << "Ooops, we have a shard for which we believe to be the "
                     "leader,"
                     " but the Plan says otherwise, we do not report in "
                     "Current "
                     "about this, database: "
                  << dbName << ", shard: " << shName;
              continue;
            }

            TRI_ASSERT(shardMap.contains(shName)) << shardMap;
            auto const [localCollectionInfo, shardInSync, shardReplicated] =
                assembleLocalCollectionInfo(df, shSlice, shardMap.at(shName),
                                            dbName, shName, serverId, allErrors,
                                            replicationVersion, localLogs);
            // Collection no longer exists
            TRI_ASSERT(!localCollectionInfo.slice().isNone());
            if (localCollectionInfo.slice().isEmptyObject() ||
                localCollectionInfo.slice().isNone()) {
              continue;
            }

            shardStats.numLeaderShards += 1;
            if (!shardInSync) {
              shardStats.numOutOfSyncShards += 1;
            }
            if (!shardReplicated) {
              shardStats.numNotReplicated += 1;
            }

            auto cp = std::vector<std::string>{AgencyCommHelper::path(),
                                               CURRENT,
                                               COLLECTIONS,
                                               dbName,
                                               colName,
                                               shName};
            auto inCurrent = cur.isObject() && cur.hasKey(cp);

            if (!inCurrent ||
                !equivalent(localCollectionInfo.slice(), cur.get(cp))) {
              report.add(VPackValue(CURRENT_COLLECTIONS + dbName + "/" +
                                    colName + "/" + shName));

              {
                VPackObjectBuilder o(&report);
                report.add(OP, VP_SET);
                // Report new current entry ...
                report.add("payload", localCollectionInfo.slice());
                // ... if and only if plan for this shard has changed in the
                // meantime Add a precondition:
                report.add(VPackValue("preconditions"));
                {
                  VPackObjectBuilder p(&report);
                  report.add(PLAN_COLLECTIONS + dbName + "/" + colName +
                                 "/shards/" + shName,
                             thePlanList);

                  if (replicationVersion == replication::Version::TWO) {
                    // Precondition that the replicated log corresponding to
                    // this shard is still leader in plan. We verify this by
                    // ensuring the term has not changed.
                    TRI_ASSERT(localTerm.has_value())
                        << "The replicated log leader must have established "
                           "leadership before reporting to Current; dbname: "
                        << dbName << " shard " << shName;
                    report.add(PLAN_REPLICATED_LOGS + dbName + "/" +
                                   to_string(localTerm->first) + "/" +
                                   StaticStrings::CurrentTerm + "/" +
                                   StaticStrings::Term,
                               VPackValue(localTerm->second));
                  }
                }
              }
            }
          } catch (std::exception const& ex) {
            LOG_TOPIC("cc837", WARN, Logger::MAINTENANCE)
                << "caught exception in Maintenance for database '" << dbName
                << "': " << ex.what();
            throw;
          }
        } else {  // Follower
          // Skip this update for replication2 databases
          if (cur.isObject() &&
              replicationVersion != replication::Version::TWO) {
            try {
              auto servers = std::vector<std::string>{AgencyCommHelper::path(),
                                                      CURRENT,
                                                      COLLECTIONS,
                                                      dbName,
                                                      colName,
                                                      shName,
                                                      SERVERS};
              auto s = cur.get(servers);
              if (s.isArray() && s[0].copyString() == serverId) {
                // We are in the situation after a restart, that we do not know
                // who the leader is because FollowerInfo is not updated yet.
                // Hence, in the case we are the Leader in Plan but do not
                // know it yet, do nothing here.
                if (shSlice.get("theLeaderTouched").isTrue()) {
                  // we were previously leader and we are done resigning.
                  // update current and let supervision handle the rest, however
                  // check that we are in the Plan a leader which is supposed to
                  // resign and add a precondition that this is still the case:

                  auto const planPath =
                      std::vector<std::string>{colName, "shards", shName};
                  if (!pdb.isObject() || !pdb.hasKey(planPath)) {
                    LOG_TOPIC("65432", DEBUG, Logger::MAINTENANCE)
                        << "Ooops, we have a shard for which we believe that "
                           "we "
                           "just resigned, but the Plan does not have it any "
                           "more,"
                           " we do not report in Current about this, database: "
                        << dbName << ", shard: " << shName;
                    continue;
                  }

                  VPackSlice thePlanList = pdb.get(planPath);
                  if (!thePlanList.isArray() || thePlanList.length() == 0 ||
                      !thePlanList[0].isString() ||
                      !thePlanList[0].isEqualStringUnchecked(UNDERSCORE +
                                                             serverId)) {
                    LOG_TOPIC("99987", DEBUG, Logger::MAINTENANCE)
                        << "Ooops, we have a shard for which we believe that "
                           "we "
                           "have just resigned, but the Plan says otherwise, "
                           "we "
                           "do not report in Current about this, database: "
                        << dbName << ", shard: " << shName;
                    continue;
                  }

                  // The representation of Current that we have retrieved from
                  // the agency is not guaranteed to be up-to-date, hence the
                  // server might not be aware of its own previous writes. We
                  // have to be careful not to override Current with outdated
                  // information. The most up-to-date list of followers can be
                  // obtained from the the local collection information. In this
                  // case, it is safe to rely on it, because we are the leader
                  // and we have just resigned. No other server has been able to
                  // take over yet.
                  auto followers = getLocalFollowers(df, dbName, shName);
                  if (followers.fail()) {
                    continue;
                  }

                  VPackBuilder ns;
                  {
                    VPackArrayBuilder a(&ns);
                    ns.add(VPackValue(UNDERSCORE + serverId));
                    for (auto&& f : *followers) {
                      ns.add(VPackValue(f));
                    }
                  }
                  report.add(VPackValue(CURRENT_COLLECTIONS + dbName + "/" +
                                        colName + "/" + shName + "/" +
                                        SERVERS));

                  {
                    VPackObjectBuilder o(&report);
                    report.add(OP, VP_SET);
                    report.add("payload", ns.slice());
                    {
                      VPackObjectBuilder p(&report, "preconditions");
                      report.add(PLAN_COLLECTIONS + dbName + "/" + colName +
                                     "/shards/" + shName,
                                 thePlanList);
                    }
                  }
                }
              }
            } catch (std::exception const& ex) {
              LOG_TOPIC("8f63e", WARN, Logger::MAINTENANCE)
                  << "caught exception in Maintenance for database '" << dbName
                  << "': " << ex.what();
              throw;
            }
          }
        }
      }
    }

    // UpdateCurrentForDatabases
    try {
      VPackSlice cdb;
      if (cur.isObject()) {
        cdbpath = std::vector<std::string>{AgencyCommHelper::path(), CURRENT,
                                           DATABASES, dbName};
        cdb = cur.get(cdbpath);
      }

      if (cdb.isObject()) {
        VPackSlice myEntry = cdb.get(serverId);
        if (!myEntry.isNone()) {
          // Database no longer in Plan and local

          if (lit == local.end() && (pit == plan.end() || pdb.isNone())) {
            // This covers the case that the database is neither in Local nor in
            // Plan. It remains to make sure an error is reported to Current if
            // there is a database in the Plan but not in Local
            report.add(VPackValue(CURRENT_DATABASES + dbName + "/" + serverId));
            {
              VPackObjectBuilder o(&report);
              report.add(OP, VP_DELETE);
            }
            // We delete all under /Current/Collections/<dbName>, it does not
            // hurt if every DBserver does this, since it is an idempotent
            // operation.
            report.add(VPackValue(CURRENT_COLLECTIONS + dbName));
            {
              VPackObjectBuilder o(&report);
              report.add(OP, VP_DELETE);
            }
            // We also delete all under /Current/ReplicatedLogs/<dbName>,
            // as well as /Current/CollectionGroups/<dbName>
            // both behave the same way as /Current/Collections/<dbName>
            // and are idempotent
            report.add(VPackValue(CURRENT_REPLICATED_LOGS + dbName));
            {
              VPackObjectBuilder o(&report);
              report.add(OP, VP_DELETE);
            }
            report.add(VPackValue(CURRENT_COLLECTION_GROUPS + dbName));
            {
              VPackObjectBuilder o(&report);
              report.add(OP, VP_DELETE);
            }
          }
        }
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("999ff", WARN, Logger::MAINTENANCE)
          << "caught exception in Maintenance for database '" << dbName
          << "': " << ex.what();
      throw;
    }

    // UpdateCurrentForCollections
    try {
      std::vector<std::string> curcolpath{AgencyCommHelper::path(), CURRENT,
                                          COLLECTIONS, dbName};
      VPackSlice curcolls;
      if (cur.isObject() && cur.hasKey(curcolpath)) {
        curcolls = cur.get(curcolpath);
      }

      // UpdateCurrentForCollections (Current/Collections/Collection)
      if (curcolls.isObject() && ldb.isObject()) {
        for (auto const& collection : VPackObjectIterator(curcolls)) {
          auto const colName = collection.key.copyString();

          TRI_ASSERT(collection.value.isObject());
          for (auto const& shard : VPackObjectIterator(collection.value)) {
            TRI_ASSERT(shard.value.isObject());

            if (!pdb.isObject()) {  // This database is no longer in plan,
              continue;             // thus no shardMap exists for it
            }

            // Shard in current and has servers
            auto servers = shard.value.get(SERVERS);
            auto const shName = shard.key.copyString();

            if (servers.isArray() && servers.length() > 0  // servers in current
                && servers[0].stringView() == serverId     // we are leading
                && !ldb.hasKey(shName)  // no local collection
                &&
                !shardMap.contains(ShardID{shName})) {  // no such shard in plan
              report.add(VPackValue(CURRENT_COLLECTIONS + dbName + "/" +
                                    colName + "/" + shName));
              {
                VPackObjectBuilder o(&report);
                report.add(OP, VP_DELETE);
              }
            }
          }
        }
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("13c97", WARN, Logger::MAINTENANCE)
          << "caught exception in Maintenance for database '" << dbName
          << "': " << ex.what();
      throw;
    }

    // UpdateReplicatedLogs
    try {
      if (auto logsIter = localLogs.find(dbName);
          logsIter != std::end(localLogs)) {
        for (auto const& [id, status] : logsIter->second) {
          reportCurrentReplicatedLog(report, status, cur, id, dbName);
        }
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("84ee0", WARN, Logger::MAINTENANCE)
          << "caught exception in Maintenance for database '" << dbName
          << "': " << ex.what();
      throw;
    }
  }  // next database

  // Let's find database errors for databases which do not occur in Local
  // but in Plan:
  try {
    for (auto const& p : allErrors.databases) {
      auto const& dbName = p.first;
      if (dirty.find(dbName) != dirty.end()) {
        // Need to create an error entry:
        report.add(VPackValue(CURRENT_DATABASES + dbName + "/" + serverId));
        {
          VPackObjectBuilder o(&report);
          report.add(OP, VP_SET);
          report.add(VPackValue("payload"));
          {
            VPackObjectBuilder pp(&report);
            VPackSlice errs(static_cast<uint8_t const*>(p.second->data()));
            TRI_ASSERT(errs.isObject());
            report.add(StaticStrings::Error, errs.get(StaticStrings::Error));
            report.add(StaticStrings::ErrorNum,
                       errs.get(StaticStrings::ErrorNum));
            report.add(StaticStrings::ErrorMessage,
                       errs.get(StaticStrings::ErrorMessage));
          }
        }
      }
    }
  } catch (std::exception const& ex) {
    LOG_TOPIC("d40a3", WARN, Logger::MAINTENANCE)
        << "caught exception in Maintenance databases error reporting: "
        << ex.what();
    throw;
  }

  // Finally, let's find shard errors for shards which do not occur in
  // Local but in Plan, we need to make sure that these errors are reported
  // in Current:
  try {
    for (auto const& p : allErrors.shards) {
      // First split the key:
      std::string const& key = p.first;
      auto pos = key.find('/');
      TRI_ASSERT(pos != std::string::npos);
      std::string d = key.substr(0, pos);  // database
      if (dirty.find(d) != dirty.end()) {
        auto const pit = plan.find(d);
        if (pit == plan.end()) {
          continue;
        }
        auto const lit = local.find(d);
        auto const cit = current.find(d);

        if (lit != local.end()) {
          auto pos2 = key.find('/', pos + 1);  // collection
          TRI_ASSERT(pos2 != std::string::npos);

          std::string c = key.substr(pos + 1, pos2);
          std::string s = key.substr(pos2 + 1);  // shard name
          TRI_ASSERT(pit->second->slice().isArray());
          TRI_ASSERT(pit->second->slice().length() == 1);
          auto const pdb = pit->second->slice()[0];
          auto const ldb = lit->second->slice();

          // Now find out if the shard appears in the Plan but not in Local:
          std::vector<std::string> const planPath{
              AgencyCommHelper::path(), PLAN, COLLECTIONS, d, c, "shards", s};

          if (!pdb.isObject()) {
            LOG_TOPIC("2647d", WARN, Logger::MAINTENANCE)
                << "plan database in error reporting struct is not an object: "
                << pdb.toJson();
          }
          if (!ldb.isObject()) {
            LOG_TOPIC("8fe58", WARN, Logger::MAINTENANCE)
                << "local database in error reporting struct is not an object: "
                << ldb.toJson();
          }
          TRI_ASSERT(pdb.isObject());
          TRI_ASSERT(ldb.isObject());
          if (pdb.hasKey(planPath) && !ldb.hasKey(s)) {
            VPackSlice servers = pdb.get(planPath);
            if (servers.isArray()) {
              TRI_ASSERT(cit != current.end());

              std::vector<std::string> const curPath{
                  AgencyCommHelper::path(), CURRENT, COLLECTIONS, d, c, s};
              VPackSlice theErr(static_cast<uint8_t const*>(p.second->data()));
              TRI_ASSERT(cit->second->slice().isObject());
              if (!cit->second->slice().hasKey(curPath) ||
                  !equivalent(theErr, cit->second->slice().get(curPath))) {
                report.add(
                    VPackValue(CURRENT_COLLECTIONS + d + "/" + c + "/" + s));
                {
                  VPackObjectBuilder o(&report);
                  report.add(OP, VP_SET);
                  report.add("payload", theErr);
                }
              }
            }
          }
        }
      }
    }
  } catch (std::exception const& ex) {
    LOG_TOPIC("ceb1a", WARN, Logger::MAINTENANCE)
        << "caught exception in Maintenance shards error reporting: "
        << ex.what();
    throw;
  }

  return {};
}

void arangodb::maintenance::syncReplicatedShardsWithLeaders(
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    containers::FlatHashSet<std::string> const& dirty,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& current,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    std::string const& serverId, MaintenanceFeature& feature,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    containers::FlatHashSet<std::string>& makeDirty) {
  for (auto const& dbname : dirty) {
    auto pit = plan.find(dbname);
    VPackSlice pdb;
    if (pit != plan.end()) {
      pdb = pit->second->slice()[0];

      // Skip shard synchronization for replication2 databases
      auto const dbpath = std::vector<std::string>{AgencyCommHelper::path(),
                                                   PLAN, DATABASES, dbname};
      if (auto dbSlice = pdb.get(dbpath); !dbSlice.isNone()) {
        if (auto rv = dbSlice.get("replicationVersion"); !rv.isNone()) {
          auto version = replication::parseVersion(rv);
          TRI_ASSERT(version.ok());
          if (version.get() == replication::Version::TWO) {
            continue;
          }
        }
      }

      auto const ppath = std::vector<std::string>{AgencyCommHelper::path(),
                                                  PLAN, COLLECTIONS, dbname};
      if (!pdb.hasKey(ppath)) {
        continue;
      } else {
        pdb = pdb.get(ppath);
      }
    } else {
      continue;
    }

    auto lit = local.find(dbname);
    VPackSlice localdb;
    if (lit != local.end()) {
      localdb = lit->second->slice();
      if (!localdb.isObject()) {
        continue;
      }
    } else {
      continue;
    }

    auto cit = current.find(dbname);
    VPackSlice cdb;
    if (cit != current.end()) {
      cdb = cit->second->slice()[0];
      TRI_ASSERT(cdb.isObject());

      auto const cpath = std::vector<std::string>{AgencyCommHelper::path(),
                                                  CURRENT, COLLECTIONS, dbname};
      if (!cdb.hasKey(cpath)) {
        continue;
      } else {
        cdb = cdb.get(cpath);
      }
    } else {
      continue;
    }

    TRI_ASSERT(pdb.isObject());
    for (auto const& pcol : VPackObjectIterator(pdb)) {
      std::string_view colname = pcol.key.stringView();

      TRI_ASSERT(cdb.isObject());
      VPackSlice const cdbcol = cdb.get(colname);
      if (!cdbcol.isObject()) {
        continue;
      }

      TRI_ASSERT(pcol.value.isObject());
      for (auto const& pshrd : VPackObjectIterator(pcol.value.get(SHARDS))) {
        std::string_view const shname = pshrd.key.stringView();

        // First check if the shard is locked:
        auto it = shardActionMap.find(ShardID{shname});
        if (it != shardActionMap.end()) {
          LOG_TOPIC("aaed5", DEBUG, Logger::MAINTENANCE)
              << "Skipping SyncReplicatedShardsWithLeader for shard " << shname
              << " because it is locked by an action: " << *it->second;
          makeDirty.emplace(dbname);
          continue;
        }

        if (!localdb.hasKey(shname)) {
          // shard does not exist locally so nothing we can do at this point
          continue;
        }

        // current stuff is created by the leader this one here will just
        // bring followers in sync so just continue here
        VPackSlice const cshrd = cdbcol.get(shname);
        if (!cshrd.isObject()) {
          LOG_TOPIC("402a4", DEBUG, Logger::MAINTENANCE)
              << "Shard " << shname
              << " not in current yet. Rescheduling maintenance.";
          continue;
        }

        // Plan's servers
        VPackSlice const pservers = pshrd.value;

        // we are not planned to be a follower
        if (indexOf(pservers, serverId) <= 0) {
          continue;
        }

        // Current's servers
        VPackSlice const cservers = cshrd.get(SERVERS);

        // From above, we know that the shard exists locally. For the case
        // that we have been restarted but the leader did not notice that
        // we were gone, we must check if the leader is set correctly here
        // locally for our shard:
        VPackSlice lshard = localdb.get(shname);
        TRI_ASSERT(lshard.isObject());
        bool needsResyncBecauseOfRestart = false;
        if (lshard.isObject()) {  // just in case
          VPackSlice theLeader = lshard.get(THE_LEADER);
          if (theLeader.isString() &&
              theLeader.stringView() ==
                  maintenance::ResignShardLeadership::LeaderNotYetKnownString) {
            needsResyncBecauseOfRestart = true;
          }
        }

        // if we are considered to be in sync there is nothing to do
        if (!needsResyncBecauseOfRestart && indexOf(cservers, serverId) > 0) {
          continue;
        }

        std::string leader = pservers[0].copyString();
        std::string forcedResync =
            needsResyncBecauseOfRestart ? "true" : "false";
        std::string syncByRevision =
            pcol.value.get(StaticStrings::SyncByRevision).isTrue() ? "true"
                                                                   : "false";
        std::shared_ptr<ActionDescription> description =
            std::make_shared<ActionDescription>(
                std::map<std::string, std::string>{
                    {NAME, SYNCHRONIZE_SHARD},
                    {DATABASE, dbname},
                    {COLLECTION, std::string(colname)},
                    {SHARD, std::string(shname)},
                    {THE_LEADER, std::move(leader)},
                    {FORCED_RESYNC, std::move(forcedResync)},
                    {SYNC_BY_REVISION, syncByRevision},
                    {SHARD_VERSION, std::to_string(feature.shardVersion(
                                        std::string(shname)))}},
                SYNCHRONIZE_PRIORITY, true);
        ShardID shardName{description->get(SHARD)};
        bool ok = feature.lockShard(shardName, description);
        TRI_ASSERT(ok);
        try {
          Result res = feature.addAction(description, false);
          if (res.fail()) {
            feature.unlockShard(shardName);
          }
        } catch (std::exception const& exc) {
          feature.unlockShard(shardName);
          LOG_TOPIC("86763", INFO, Logger::MAINTENANCE)
              << "Exception caught when adding synchronize shard action, "
                 "unlocking shard "
              << shardName << " again: " << exc.what();
        }
      }
    }
  }
}

/// @brief Phase two: See, what we can report to the agency
arangodb::Result arangodb::maintenance::phaseTwo(
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& plan,
    containers::FlatHashMap<std::string,
                            std::shared_ptr<VPackBuilder const>> const& cur,
    uint64_t currentIndex, containers::FlatHashSet<std::string> const& dirty,
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        local,
    std::string const& serverId, MaintenanceFeature& feature,
    VPackBuilder& report,
    MaintenanceFeature::ShardActionMap const& shardActionMap,
    ReplicatedLogStatusMapByDatabase const& localLogs,
    ShardIdToLogIdMapByDatabase const& localShardIdToLogId) {
  auto start = std::chrono::steady_clock::now();

  MaintenanceFeature::errors_t allErrors;
  feature.copyAllErrors(allErrors);

  arangodb::Result result;
  ShardStatistics shardStats{};  // zero initialize

  report.add(VPackValue(PHASE_TWO));
  {
    VPackObjectBuilder p2(&report);

    // agency transactions
    report.add(VPackValue("agency"));
    {
      VPackObjectBuilder agency(&report);
      // Update Current
      try {
        result = reportInCurrent(feature, plan, dirty, cur, local, allErrors,
                                 serverId, report, shardStats, localLogs,
                                 localShardIdToLogId);
      } catch (std::exception const& e) {
        LOG_TOPIC("c9a75", ERR, Logger::MAINTENANCE)
            << "Error reporting in current: " << e.what();
      }
    }

    // maintenance actions
    report.add(VPackValue("actions"));
    {
      VPackObjectBuilder agency(&report);
      try {
        containers::FlatHashSet<std::string> makeDirty;
        syncReplicatedShardsWithLeaders(plan, dirty, cur, local, serverId,
                                        feature, shardActionMap, makeDirty);
        feature.addDirty(makeDirty, false);
      } catch (std::exception const& e) {
        LOG_TOPIC("7e286", ERR, Logger::MAINTENANCE)
            << "Error scheduling shards: " << e.what();
      }
    }
  }

  report.add(VPackValue("Current"));
  {
    VPackObjectBuilder p(&report);
    report.add("Index", VPackValue(currentIndex));
  }

  auto end = std::chrono::steady_clock::now();
  uint64_t total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();
  TRI_ASSERT(feature._phase2_runtime_msec != nullptr);
  feature._phase2_runtime_msec->count(total_ms);

  TRI_ASSERT(feature._shards_out_of_sync != nullptr);
  feature._shards_out_of_sync->store(shardStats.numOutOfSyncShards,
                                     std::memory_order_relaxed);
  TRI_ASSERT(feature._shards_total_count != nullptr);
  feature._shards_total_count->store(shardStats.numShards,
                                     std::memory_order_relaxed);
  TRI_ASSERT(feature._shards_leader_count != nullptr);
  feature._shards_leader_count->store(shardStats.numLeaderShards,
                                      std::memory_order_relaxed);
  TRI_ASSERT(feature._shards_not_replicated_count != nullptr);
  feature._shards_not_replicated_count->store(shardStats.numNotReplicated,
                                              std::memory_order_relaxed);

  return result;
}
