////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "Collections.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Basics/Common.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/fasthash.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Graph/GraphManager.h"
#include "Logger/LogMacros.h"
#include "RestServer/DatabaseFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardingFeature.h"
#include "Sharding/ShardingInfo.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utilities/NameValidator.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8Server/V8Context.h"
#include "VocBase/ComputedValues.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/CollectionCreationInfo.h"
#include "VocBase/Properties/PlanCollection.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>

#include <unordered_set>

using namespace arangodb;
using namespace arangodb::methods;

using Helper = arangodb::basics::VelocyPackHelper;

namespace {

bool checkIfDefinedAsSatellite(VPackSlice const& properties) {
  if (properties.hasKey(StaticStrings::ReplicationFactor)) {
    if (properties.get(StaticStrings::ReplicationFactor).isNumber()) {
      auto replFactor =
          properties.get(StaticStrings::ReplicationFactor).getNumber<size_t>();
      if (replFactor == 0) {
        return true;
      }
    } else if (properties.get(StaticStrings::ReplicationFactor).isString()) {
      if (properties.get(StaticStrings::ReplicationFactor)
              .isEqualString(StaticStrings::Satellite)) {
        return true;
      }
    }
  }
  return false;
}

bool isLocalCollection(CollectionCreationInfo const& info) {
  return (!ServerState::instance()->isCoordinator() &&
          Helper::stringUInt64(
              info.properties.get(StaticStrings::DataSourcePlanId)) == 0);
}

bool isSystemName(CollectionCreationInfo const& info) {
  return NameValidator::isSystemName(info.name);
}

Result validateCreationInfo(CollectionCreationInfo const& info,
                            TRI_vocbase_t& vocbase,
                            bool isSingleServerSmartGraph,
                            bool enforceReplicationFactor,
                            bool isLocalCollection, bool isSystemName,
                            bool allowSystem = false) {
  // check whether the name of the collection is valid
  bool extendedNames = vocbase.server()
                           .getFeature<DatabaseFeature>()
                           .extendedNamesForCollections();
  if (!CollectionNameValidator::isAllowedName(allowSystem, extendedNames,
                                              info.name)) {
    events::CreateCollection(vocbase.name(), info.name,
                             TRI_ERROR_ARANGO_ILLEGAL_NAME);
    return {TRI_ERROR_ARANGO_ILLEGAL_NAME};
  }

  // check the collection type in _info
  if (info.collectionType != TRI_col_type_e::TRI_COL_TYPE_DOCUMENT &&
      info.collectionType != TRI_col_type_e::TRI_COL_TYPE_EDGE) {
    events::CreateCollection(vocbase.name(), info.name,
                             TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
    return {TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID};
  }

  // validate number of shards and replication factor
  if (ServerState::instance()->isCoordinator() || isSingleServerSmartGraph) {
    Result res = ShardingInfo::validateShardsAndReplicationFactor(
        info.properties, vocbase.server(), enforceReplicationFactor);
    if (res.fail()) {
      return res;
    }
  }

  std::vector<std::string> shardKeys;
  if (ServerState::instance()->isCoordinator()) {
    bool isSmart = basics::VelocyPackHelper::getBooleanValue(
        info.properties, StaticStrings::IsSmart, false);
    size_t replicationFactor = 1;
    Result res = ShardingInfo::extractReplicationFactor(
        info.properties, isSmart, replicationFactor);
    if (res.ok()) {
      // extract shard keys
      res = ShardingInfo::extractShardKeys(info.properties, replicationFactor,
                                           shardKeys);
    }
    if (res.fail()) {
      return res;
    }
  }

  // validate computed values
  auto result = ComputedValues::buildInstance(
      vocbase, shardKeys, info.properties.get(StaticStrings::ComputedValues));
  if (result.fail()) {
    return result.result();
  }

  // All collections on a single server should be local collections.
  // A Coordinator should never have local collections.
  // On an Agent, all collections should be local collections.
  // On a DBServer, the only local collections should be system collections
  // (like _statisticsRaw). Non-local (system or not) collections are shards,
  // so don't have system-names, even if they are system collections!
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_SINGLE:
      TRI_ASSERT(isLocalCollection);
      break;
    case ServerState::ROLE_DBSERVER:
      TRI_ASSERT(isLocalCollection == isSystemName);
      break;
    case ServerState::ROLE_COORDINATOR:
      TRI_ASSERT(!isLocalCollection);
      break;
    case ServerState::ROLE_AGENT:
      TRI_ASSERT(isLocalCollection);
      break;
    case ServerState::ROLE_UNDEFINED:
      TRI_ASSERT(false);
  }

  if (isLocalCollection && !isSingleServerSmartGraph) {
    // the combination "isSmart" and replicationFactor "satellite" does not make
    // any sense. note: replicationFactor "satellite" can also be expressed as
    // replicationFactor 0.
    VPackSlice s = info.properties.get(StaticStrings::IsSmart);
    auto replicationFactorSlice =
        info.properties.get(StaticStrings::ReplicationFactor);
    if (s.isTrue() &&
        ((replicationFactorSlice.isNumber() &&
          replicationFactorSlice.getNumber<int>() == 0) ||
         (replicationFactorSlice.isString() &&
          replicationFactorSlice.stringView() == StaticStrings::Satellite))) {
      return {TRI_ERROR_BAD_PARAMETER,
              "invalid combination of 'isSmart' and 'satellite' "
              "replicationFactor"};
    }
  }

  return {};
}

// validate collection parameters. if the validation fails, it will audit-log
// the failure.
Result validateAllCollectionsInfo(
    TRI_vocbase_t& vocbase, std::vector<CollectionCreationInfo> const& infos,
    bool allowSystem, bool allowEnterpriseCollectionsOnSingleServer,
    bool enforceReplicationFactor) {
  Result res;

  for (auto const& info : infos) {
    // If the PlanId is not set, we either are on a single server, or this is
    // a local collection in a cluster; which means, it is neither a user-facing
    // collection (as seen on a Coordinator), nor a shard (on a DBServer).

    // validate the information of the collection to be created
    res = validateCreationInfo(
        info, vocbase, allowEnterpriseCollectionsOnSingleServer,
        enforceReplicationFactor, isLocalCollection(info), isSystemName(info),
        allowSystem);
    if (res.fail()) {
      events::CreateCollection(vocbase.name(), info.name, res.errorNumber());
      return res;
    }
  }

  return {};
}

// Returns a builder that combines the information from infos and cluster
// related information.
VPackBuilder createCollectionProperties(
    TRI_vocbase_t const& vocbase,
    std::vector<CollectionCreationInfo> const& infos,
    bool allowEnterpriseCollectionsOnSingleServer) {
  StorageEngine& engine =
      vocbase.server().getFeature<EngineSelectorFeature>().engine();
  VPackBuilder builder;
  VPackBuilder helper;

  builder.openArray();

  for (auto const& info : infos) {
    TRI_ASSERT(builder.isOpenArray());
    TRI_ASSERT(info.properties.isObject());

    helper.clear();
    helper.openObject();
    helper.add(arangodb::StaticStrings::DataSourceType,
               VPackValue(static_cast<int>(info.collectionType)));
    helper.add(arangodb::StaticStrings::DataSourceName, VPackValue(info.name));

    // generate a rocksdb collection object id in case it does not exist
    if (allowEnterpriseCollectionsOnSingleServer) {
      engine.addParametersForNewCollection(helper, info.properties);
    }

    bool addUseRevs = ServerState::instance()->isSingleServerOrCoordinator();
    bool useRevs =
        !info.properties
             .get(arangodb::StaticStrings::UsesRevisionsAsDocumentIds)
             .isFalse() &&
        LogicalCollection::currentVersion() >= LogicalCollection::Version::v37;

    if (addUseRevs) {
      helper.add(arangodb::StaticStrings::UsesRevisionsAsDocumentIds,
                 arangodb::velocypack::Value(useRevs));
    }

    if (!isLocalCollection(info) || allowEnterpriseCollectionsOnSingleServer) {
      auto const& cl = vocbase.server().getFeature<ClusterFeature>();
      auto replicationFactorSlice =
          info.properties.get(StaticStrings::ReplicationFactor);

      if (replicationFactorSlice.isNone()) {
        auto factor = vocbase.replicationFactor();
        if (factor > 0 && isSystemName(info)) {
          factor = std::max(vocbase.replicationFactor(),
                            cl.systemReplicationFactor());
        }
        helper.add(StaticStrings::ReplicationFactor, VPackValue(factor));
      };

      bool isSatellite = checkIfDefinedAsSatellite(info.properties);

      if (!isSystemName(info)) {
        // system-collections will be sharded normally. only user collections
        // will get the forced sharding
        if (vocbase.server().getFeature<ClusterFeature>().forceOneShard() ||
            vocbase.isOneShard()) {
          // force one shard, and force distributeShardsLike to be "_graphs"
          helper.add(StaticStrings::NumberOfShards, VPackValue(1));
          if (!isSatellite) {
            // SatelliteCollections must not be sharded like a
            // non-SatelliteCollection.
            helper.add(StaticStrings::DistributeShardsLike,
                       VPackValue(vocbase.shardingPrototypeName()));
          }
        }
      }

      if (!isSatellite) {
        auto writeConcernSlice =
            info.properties.get(StaticStrings::WriteConcern);
        if (writeConcernSlice
                .isNone()) {  // "minReplicationFactor" deprecated in 3.6
          writeConcernSlice =
              info.properties.get(StaticStrings::MinReplicationFactor);
        }

        if (writeConcernSlice.isNone()) {
          helper.add(StaticStrings::MinReplicationFactor,
                     VPackValue(vocbase.writeConcern()));
          helper.add(StaticStrings::WriteConcern,
                     VPackValue(vocbase.writeConcern()));
        }
      }
    } else {  // single server or agent
      bool distributionSet = false;
#ifdef USE_ENTERPRISE
      TRI_ASSERT(ServerState::instance()->isSingleServer() ||
                 ServerState::instance()->isAgent());
      // Special case for sharded graphs with satellites
      if (info.properties.hasKey(StaticStrings::DistributeShardsLike)) {
        // 1.) Either we distribute like another satellite collection
        auto distributeShardsLikeSlice =
            info.properties.get(StaticStrings::DistributeShardsLike);
        if (distributeShardsLikeSlice.isString()) {
          auto distributeShardsLikeColName =
              distributeShardsLikeSlice.copyString();
          auto coll = vocbase.lookupCollection(distributeShardsLikeColName);
          if (coll != nullptr && coll->isSatellite()) {
            helper.add(StaticStrings::DistributeShardsLike,
                       VPackValue(distributeShardsLikeColName));
            helper.add(StaticStrings::ReplicationFactor,
                       VPackValue(coll->replicationFactor()));
            distributionSet = true;
          }
        }
      } else if (info.properties.hasKey(StaticStrings::ReplicationFactor)) {
        // 2.) OR the collection itself is a satellite collection
        if (info.properties.get(StaticStrings::ReplicationFactor).isString() &&
            info.properties.get(StaticStrings::ReplicationFactor)
                    .copyString() == StaticStrings::Satellite) {
          distributionSet = true;
        }
      }
#endif
      if (!distributionSet) {
        helper.add(
            StaticStrings::DistributeShardsLike,
            VPackSlice::nullSlice());  // delete empty string from info slice
        helper.add(StaticStrings::ReplicationFactor, VPackSlice::nullSlice());
      }
      helper.add(StaticStrings::MinReplicationFactor,
                 VPackSlice::nullSlice());  // deprecated
      helper.add(StaticStrings::WriteConcern, VPackSlice::nullSlice());
    }

    helper.close();

    VPackBuilder merged =
        VPackCollection::merge(info.properties, helper.slice(), false, true);

    bool haveShardingFeature = (ServerState::instance()->isCoordinator() ||
                                allowEnterpriseCollectionsOnSingleServer) &&
                               vocbase.server().hasFeature<ShardingFeature>();
    if (haveShardingFeature &&
        !info.properties.get(StaticStrings::ShardingStrategy).isString()) {
      // NOTE: We need to do this in a second merge as the feature call requires
      // the DataSourceType to be set in the JSON, which has just been done by
      // the call above.
      helper.clear();
      helper.openObject();
      helper.add(StaticStrings::ShardingStrategy,
                 VPackValue(vocbase.server()
                                .getFeature<ShardingFeature>()
                                .getDefaultShardingStrategyForNewCollection(
                                    merged.slice())));
      helper.close();
      merged =
          VPackCollection::merge(merged.slice(), helper.slice(), false, true);
    }

    builder.add(merged.slice());
  }

  TRI_ASSERT(builder.isOpenArray());
  builder.close();

  return builder;
}
}  // namespace

Collections::Context::Context(std::shared_ptr<LogicalCollection> coll)
    : _coll(std::move(coll)), _trx(nullptr), _responsibleForTrx(true) {}

Collections::Context::Context(std::shared_ptr<LogicalCollection> coll,
                              transaction::Methods* trx)
    : _coll(std::move(coll)), _trx(trx), _responsibleForTrx(false) {
  TRI_ASSERT(_trx != nullptr);
}

Collections::Context::~Context() {
  if (_responsibleForTrx && _trx != nullptr) {
    delete _trx;
  }
}

transaction::Methods* Collections::Context::trx(AccessMode::Type const& type,
                                                bool embeddable,
                                                bool forceLoadCollection) {
  if (_responsibleForTrx && _trx == nullptr) {
    auto ctx = transaction::V8Context::CreateWhenRequired(_coll->vocbase(),
                                                          embeddable);
    auto trx = std::make_unique<SingleCollectionTransaction>(ctx, *_coll, type);
    if (!trx) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_OUT_OF_MEMORY,
                                     "Cannot create Transaction");
    }

    if (!forceLoadCollection) {
      // we actually need this hint here, so that the collection is not
      // loaded if it has status unloaded.
      trx->addHint(transaction::Hints::Hint::NO_USAGE_LOCK);
    }

    Result res = trx->begin();

    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
    _trx = trx.release();
  }
  // ADD asserts for running state and locking issues!
  return _trx;
}

std::shared_ptr<LogicalCollection> Collections::Context::coll() const {
  return _coll;
}

/// @brief check if a name belongs to a collection
bool Collections::hasName(CollectionNameResolver const& resolver,
                          LogicalCollection const& collection,
                          std::string const& collectionName) {
  if (collectionName == collection.name()) {
    return true;
  }

  if (collectionName == std::to_string(collection.id().id())) {
    return true;
  }

  // Shouldn't it just be: If we are on DBServer we also have to check for
  // global ID name and cid should be the shard.
  if (ServerState::instance()->isCoordinator()) {
    if (collectionName == resolver.getCollectionNameCluster(collection.id())) {
      return true;
    }
    return false;
  }

  return (collectionName == resolver.getCollectionName(collection.id()));
}

std::vector<std::shared_ptr<LogicalCollection>> Collections::sorted(
    TRI_vocbase_t& vocbase) {
  std::vector<std::shared_ptr<LogicalCollection>> result;

  if (ServerState::instance()->isCoordinator()) {
    auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
    std::vector<std::shared_ptr<LogicalCollection>> colls =
        ci.getCollections(vocbase.name());

    result.reserve(colls.size());

    for (std::shared_ptr<LogicalCollection>& c : colls) {
      if (!c->deleted()) {
        result.emplace_back(std::move(c));
      }
    }
  } else {
    result = vocbase.collections(false);
  }

  std::sort(result.begin(), result.end(),
            [](std::shared_ptr<LogicalCollection> const& lhs,
               std::shared_ptr<LogicalCollection> const& rhs) -> bool {
              return arangodb::basics::StringUtils::tolower(lhs->name()) <
                     arangodb::basics::StringUtils::tolower(rhs->name());
            });

  return result;
}

void Collections::enumerate(
    TRI_vocbase_t* vocbase,
    std::function<void(std::shared_ptr<LogicalCollection> const&)> const&
        func) {
  if (ServerState::instance()->isCoordinator()) {
    auto& ci = vocbase->server().getFeature<ClusterFeature>().clusterInfo();
    std::vector<std::shared_ptr<LogicalCollection>> colls =
        ci.getCollections(vocbase->name());

    for (std::shared_ptr<LogicalCollection> const& c : colls) {
      if (!c->deleted()) {
        func(c);
      }
    }
  } else {
    for (auto const& c : vocbase->collections(false)) {
      if (!c->deleted()) {
        func(c);
      }
    }
  }
}

/*static*/ Result methods::Collections::lookup(  // find collection
    TRI_vocbase_t const& vocbase,                // vocbase to search
    std::string const& name,                     // collection name
    std::shared_ptr<LogicalCollection>& ret) {
  if (name.empty()) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  if (ServerState::instance()->isCoordinator()) {
    try {
      if (!vocbase.server().hasFeature<ClusterFeature>()) {
        return arangodb::Result(  // result
            TRI_ERROR_INTERNAL,   // code
            "failure to find 'ClusterInfo' instance while searching for "
            "collection"  // message
        );
      }
      auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();

      auto coll = ci.getCollectionNT(vocbase.name(), name);
      if (coll) {
        // check authentication after ensuring the collection exists
        if (!ExecContext::current().canUseCollection(
                vocbase.name(), coll->name(), auth::Level::RO)) {
          return Result(TRI_ERROR_FORBIDDEN,
                        "No access to collection '" + name + "'");
        }

        ret = std::move(coll);

        return Result();
      } else {
        return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                      "collection not found");
      }
    } catch (basics::Exception const& ex) {
      return Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      return Result(TRI_ERROR_INTERNAL,
                    "internal error during collection lookup");
    }

    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  auto coll = vocbase.lookupCollection(name);

  if (coll != nullptr) {
    // check authentication after ensuring the collection exists
    if (!ExecContext::current().canUseCollection(vocbase.name(), coll->name(),
                                                 auth::Level::RO)) {
      return Result(TRI_ERROR_FORBIDDEN,
                    "No access to collection '" + name + "'");
    }
    try {
      ret = std::move(coll);
    } catch (basics::Exception const& ex) {
      return Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      return Result(
          TRI_ERROR_INTERNAL,
          "internal error during collection lookup - canUseCollection");
    }

    return Result();
  }

  return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
}

[[nodiscard]] arangodb::ResultT<std::vector<std::shared_ptr<LogicalCollection>>>
Collections::create(         // create collection
    TRI_vocbase_t& vocbase,  // collection vocbase
    OperationOptions const& options,
    std::vector<PlanCollection> collections,  // Collections to create
    bool createWaitsForSyncReplication,       // replication wait flag
    bool enforceReplicationFactor,            // replication factor flag
    bool isNewDatabase, bool allowEnterpriseCollectionsOnSingleServer,
    bool isRestore) {
  // Let's first check if we are allowed to create the collections
  ExecContext const& exec = options.context();
  if (!exec.canUseDatabase(vocbase.name(), auth::Level::RW)) {
    for (auto const& col : collections) {
      events::CreateCollection(vocbase.name(), col.name, TRI_ERROR_FORBIDDEN);
    }
    return arangodb::Result(                             // result
        TRI_ERROR_FORBIDDEN,                             // code
        "cannot create collection in " + vocbase.name()  // message
    );
  }

  // TODO: Discuss should this be Prod Assert?
  // We are trying to protect against someone creating collections
  // in a database that is to be deleted.
  // Or should this be a possible error?
  TRI_ASSERT(!vocbase.isDangling());

  PlanCollection::DatabaseConfiguration config(vocbase);
  config.enforceReplicationFactor = enforceReplicationFactor;

  ClusterInfo& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
  for (auto& col : collections) {
    if (!col.distributeShardsLike.empty()) {
      // Apply distributeShardsLike overwrites
      // TODO: Make this piece unit-testable.
      CollectionNameResolver resolver(vocbase);
      auto myColToDistributeLike = resolver.getCollection(col.distributeShardsLike);
      if (myColToDistributeLike == nullptr) {
        return Result{
          TRI_ERROR_CLUSTER_UNKNOWN_DISTRIBUTESHARDSLIKE,
            "Collection not found: " + col.distributeShardsLike +
            " in database " + vocbase.name()};
      }
      col.numberOfShards = myColToDistributeLike->numberOfShards();
      col.replicationFactor = myColToDistributeLike->replicationFactor();
      // TODO: writeConcern is inherited when using replication2
      // col.writeConcern = myColToDistributeLike->writeConcern();
    }
    // TODO: Maybe we can do this within the parsing Step already.
    // but that is a cleanup for later.
    auto res = col.validateDatabaseConfiguration(config);
    if (res.fail()) {
      for (auto const& reportCollection : collections) {
        events::CreateCollection(vocbase.name(), reportCollection.name,
                                 res.errorNumber());
      }
      return res;
    }

    // make sure we have enough servers available for the replication
    // factor
    if (config.enforceReplicationFactor &&
        ServerState::instance()->isCoordinator() &&
        col.replicationFactor > ci.getCurrentDBServers().size()) {
      return Result(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
    }

    {
      // Only validate computed values
      // we do not make use of the actual ComputedValueExecutor here
      // TODO: Can be replaced by a two step way inspect -> ComputedValuesInput
      // -> ComputedValues
      auto result = ComputedValues::buildInstance(vocbase, col.shardKeys,
                                                  col.computedValues.slice());
      if (result.fail()) {
        return result.result();
      }
    }
  }

  arangodb::ResultT<std::vector<std::shared_ptr<LogicalCollection>>> results{
      TRI_ERROR_INTERNAL};

  if (ServerState::instance()->isCoordinator()) {
    // Here we do have a cluster setup. In that case, we will create many
    // collections in one go (batch-wise).
    results = ClusterMethods::createCollectionsOnCoordinator(
        vocbase, collections, false, createWaitsForSyncReplication,
        enforceReplicationFactor, isNewDatabase);
    if (results.fail()) {
      for (auto const& info : collections) {
        events::CreateCollection(vocbase.name(), info.name,
                                 results.errorNumber());
      }
      return results;
    }
    if (results->empty()) {
      // TODO: CHeck if this can really happen after refactoring is completed
      // We may be able to guarantee error when nothing could be done.
      for (auto const& info : collections) {
        events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_INTERNAL);
      }
      return Result(TRI_ERROR_INTERNAL, "createCollectionsOnCoordinator");
    }
  } else {
    TRI_ASSERT(ServerState::instance()->isSingleServer() ||
               ServerState::instance()->isDBServer() ||
               ServerState::instance()->isAgent());
    // Here we do have a single server setup, or we're either on a DBServer
    // / Agency. In that case, we're not batching collection creating.
    // Therefore, we need to iterate over the infoSlice and create each
    // collection one by one.
    results = vocbase.createCollections(
        collections, allowEnterpriseCollectionsOnSingleServer);
    if (results.fail()) {
      return results;
    }
  }

  // Grant access to the collections.
  // This is identical on cluster and SingleServer
  try {
    // in case of success we grant the creating user RW access
    auth::UserManager* um = AuthenticationFeature::instance()->userManager();
    if (um != nullptr && !exec.isSuperuser() && !isRestore) {
      // this should not fail, we can not get here without database RW access
      // however, there may be races for updating the users account, so we try
      // a few times in case of a conflict
      int tries = 0;
      while (true) {
        Result r = um->updateUser(exec.user(), [&](auth::User& entry) {
          for (auto const& col : *results) {
            // do not grant rights on system collections
            if (!col->system()) {
              entry.grantCollection(vocbase.name(), col->name(),
                                    auth::Level::RW);
            }
          }
          return TRI_ERROR_NO_ERROR;
        });
        if (r.ok() || r.is(TRI_ERROR_USER_NOT_FOUND) ||
            r.is(TRI_ERROR_USER_EXTERNAL)) {
          // it seems to be allowed to created collections with an unknown user
          break;
        }
        if (!r.is(TRI_ERROR_ARANGO_CONFLICT) || ++tries == 10) {
          // This could be 116bb again, as soon as "old code" is removed
          LOG_TOPIC("116bc", WARN, Logger::AUTHENTICATION)
              << "Updating user failed with error: " << r.errorMessage()
              << ". giving up!";
          for (auto const& col : *results) {
            events::CreateCollection(vocbase.name(), col->name(),
                                     r.errorNumber());
          }
          return r;
        }
        // try again in case of conflict
        // This could be ff123 again, as soon as "old code" is removed
        LOG_TOPIC("ff124", TRACE, Logger::AUTHENTICATION)
            << "Updating user failed with error: " << r.errorMessage()
            << ". trying again";
      }
    }
  } catch (basics::Exception const& ex) {
    for (auto const& info : collections) {
      events::CreateCollection(vocbase.name(), info.name, ex.code());
    }
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    for (auto const& info : collections) {
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_INTERNAL);
    }
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    for (auto const& info : collections) {
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_INTERNAL);
    }
    return Result(TRI_ERROR_INTERNAL, "cannot create collection");
  }
  for (auto const& info : collections) {
    if (!ServerState::instance()->isSingleServer()) {
      // don't log here (again) for single servers, because on the single
      // server we will log the creation of each collection inside
      // vocbase::createCollectionWorker
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_NO_ERROR);
    }

    // TODO: update auditLog reporting
    OperationResult result(Result(), info.toCollectionsCreate().steal(),
                           options);
    events::PropertyUpdateCollection(vocbase.name(), info.name, result);
  }

  return results;
}

/*static*/ arangodb::Result Collections::create(  // create collection
    TRI_vocbase_t& vocbase,                       // collection vocbase
    OperationOptions const& options,
    std::string const& name,                 // collection name
    TRI_col_type_e collectionType,           // collection type
    arangodb::velocypack::Slice properties,  // collection properties
    bool createWaitsForSyncReplication,      // replication wait flag
    bool enforceReplicationFactor,           // replication factor flag
    bool isNewDatabase,
    std::shared_ptr<LogicalCollection>& ret,  // invoke on collection creation
    bool allowSystem, bool allowEnterpriseCollectionsOnSingleServer,
    bool isRestore) {
  if (name.empty()) {
    events::CreateCollection(vocbase.name(), name,
                             TRI_ERROR_ARANGO_ILLEGAL_NAME);
    return TRI_ERROR_ARANGO_ILLEGAL_NAME;
  } else if (collectionType != TRI_col_type_e::TRI_COL_TYPE_DOCUMENT &&
             collectionType != TRI_col_type_e::TRI_COL_TYPE_EDGE) {
    events::CreateCollection(vocbase.name(), name,
                             TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
    return TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID;
  }
  std::vector<CollectionCreationInfo> infos{{name, collectionType, properties}};
  std::vector<std::shared_ptr<LogicalCollection>> collections;
  Result res =
      create(vocbase, options, infos, createWaitsForSyncReplication,
             enforceReplicationFactor, isNewDatabase, nullptr, collections,
             allowSystem, allowEnterpriseCollectionsOnSingleServer, isRestore);
  if (res.ok() && collections.size() > 0) {
    ret = std::move(collections[0]);
  }
  return res;
}

Result Collections::create(
    TRI_vocbase_t& vocbase, OperationOptions const& options,
    std::vector<CollectionCreationInfo> const& infos,
    bool createWaitsForSyncReplication, bool enforceReplicationFactor,
    bool isNewDatabase,
    std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike,
    std::vector<std::shared_ptr<LogicalCollection>>& ret, bool allowSystem,
    bool allowEnterpriseCollectionsOnSingleServer, bool isRestore) {
  ExecContext const& exec = options.context();
  if (!exec.canUseDatabase(vocbase.name(), auth::Level::RW)) {
    for (auto const& info : infos) {
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_FORBIDDEN);
    }
    return arangodb::Result(                             // result
        TRI_ERROR_FORBIDDEN,                             // code
        "cannot create collection in " + vocbase.name()  // message
    );
  }

  TRI_ASSERT(!vocbase.isDangling());

  // validate information from every element of infos.
  // if the validation fails, it will audit-log the failure.
  Result res = validateAllCollectionsInfo(
      vocbase, infos, allowSystem, allowEnterpriseCollectionsOnSingleServer,
      enforceReplicationFactor);
  if (res.fail()) {
    return res;
  }

  // construct a builder that contains information from all elements of infos
  // and cluster related information
  VPackBuilder builder = createCollectionProperties(
      vocbase, infos, allowEnterpriseCollectionsOnSingleServer);

  VPackSlice infoSlice = builder.slice();

  TRI_ASSERT(infoSlice.isArray());
  TRI_ASSERT(infoSlice.length() >= 1);
  TRI_ASSERT(infoSlice.length() == infos.size());

  std::vector<std::shared_ptr<LogicalCollection>> collections;
  collections.reserve(infoSlice.length());

  try {
    if (ServerState::instance()->isCoordinator()) {
      // Here we do have a cluster setup. In that case, we will create many
      // collections in one go (batch-wise).
      collections = ClusterMethods::createCollectionsOnCoordinator(
          vocbase, infoSlice, false, createWaitsForSyncReplication,
          enforceReplicationFactor, isNewDatabase, colToDistributeShardsLike);

      if (collections.empty()) {
        for (auto const& info : infos) {
          events::CreateCollection(vocbase.name(), info.name,
                                   TRI_ERROR_INTERNAL);
        }
        return Result(TRI_ERROR_INTERNAL, "createCollectionsOnCoordinator");
      }
    } else {
      TRI_ASSERT(ServerState::instance()->isSingleServer() ||
                 ServerState::instance()->isDBServer() ||
                 ServerState::instance()->isAgent());
      // Here we do have a single server setup, or we're either on a DBServer /
      // Agency. In that case, we're not batching collection creating.
      // Therefore, we need to iterate over the infoSlice and create each
      // collection one by one.
      collections = vocbase.createCollections(
          infoSlice, allowEnterpriseCollectionsOnSingleServer);
    }
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL, "cannot create collection");
  }

  // Grant access to the collections.
  // This is identical on cluster and SingleServer
  try {
    // in case of success we grant the creating user RW access
    auth::UserManager* um = AuthenticationFeature::instance()->userManager();
    if (um != nullptr && !exec.isSuperuser() && !isRestore) {
      // this should not fail, we can not get here without database RW access
      // however, there may be races for updating the users account, so we try
      // a few times in case of a conflict
      int tries = 0;
      while (true) {
        Result r = um->updateUser(exec.user(), [&](auth::User& entry) {
          for (auto const& col : collections) {
            // do not grant rights on system collections
            if (!col->system()) {
              entry.grantCollection(vocbase.name(), col->name(),
                                    auth::Level::RW);
            }
          }
          return TRI_ERROR_NO_ERROR;
        });
        if (r.ok() || r.is(TRI_ERROR_USER_NOT_FOUND) ||
            r.is(TRI_ERROR_USER_EXTERNAL)) {
          // it seems to be allowed to created collections with an unknown user
          break;
        }
        if (!r.is(TRI_ERROR_ARANGO_CONFLICT) || ++tries == 10) {
          LOG_TOPIC("116bb", WARN, Logger::AUTHENTICATION)
              << "Updating user failed with error: " << r.errorMessage()
              << ". giving up!";
          for (auto const& col : collections) {
            events::CreateCollection(vocbase.name(), col->name(),
                                     r.errorNumber());
          }
          return r;
        }
        // try again in case of conflict
        LOG_TOPIC("ff123", TRACE, Logger::AUTHENTICATION)
            << "Updating user failed with error: " << r.errorMessage()
            << ". trying again";
      }
    }
    ret = std::move(collections);
  } catch (basics::Exception const& ex) {
    for (auto const& info : infos) {
      events::CreateCollection(vocbase.name(), info.name, ex.code());
    }
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    for (auto const& info : infos) {
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_INTERNAL);
    }
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    for (auto const& info : infos) {
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_INTERNAL);
    }
    return Result(TRI_ERROR_INTERNAL, "cannot create collection");
  }
  for (auto const& info : infos) {
    if (!ServerState::instance()->isSingleServer()) {
      // don't log here (again) for single servers, because on the single
      // server we will log the creation of each collection inside
      // vocbase::createCollectionWorker
      events::CreateCollection(vocbase.name(), info.name, TRI_ERROR_NO_ERROR);
    }
    velocypack::Builder builder(info.properties);
    OperationResult result(Result(), builder.steal(), options);
    events::PropertyUpdateCollection(vocbase.name(), info.name, result);
  }

  return {};
}

void Collections::createSystemCollectionProperties(
    std::string const& collectionName, VPackBuilder& bb,
    TRI_vocbase_t const& vocbase) {
  uint32_t defaultReplicationFactor = vocbase.replicationFactor();
  uint32_t defaultWriteConcern = vocbase.writeConcern();

  if (vocbase.server().hasFeature<ClusterFeature>()) {
    defaultReplicationFactor =
        std::max(defaultReplicationFactor, vocbase.server()
                                               .getFeature<ClusterFeature>()
                                               .systemReplicationFactor());
  }

  {
    VPackObjectBuilder scope(&bb);
    bb.add(StaticStrings::DataSourceSystem, VPackSlice::trueSlice());
    bb.add(StaticStrings::WaitForSyncString, VPackSlice::falseSlice());
    bb.add(StaticStrings::ReplicationFactor,
           VPackValue(defaultReplicationFactor));
    bb.add(StaticStrings::MinReplicationFactor,
           VPackValue(defaultWriteConcern));  // deprecated
    bb.add(StaticStrings::WriteConcern, VPackValue(defaultWriteConcern));

    // that forces all collections to be on the same physical DBserver
    if (vocbase.isSystem()) {
      if (collectionName != StaticStrings::UsersCollection) {
        bb.add(StaticStrings::DistributeShardsLike,
               VPackValue(StaticStrings::UsersCollection));
      }
    } else {
      if (collectionName != StaticStrings::GraphsCollection) {
        bb.add(StaticStrings::DistributeShardsLike,
               VPackValue(StaticStrings::GraphsCollection));
      }
    }
  }
}

/*static*/ Result Collections::createSystem(
    TRI_vocbase_t& vocbase, OperationOptions const& options,
    std::string const& name, bool isNewDatabase,
    std::shared_ptr<LogicalCollection>& createdCollection) {
  Result res = methods::Collections::lookup(vocbase, name, createdCollection);

  if (res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
    VPackBuilder bb;
    createSystemCollectionProperties(name, bb, vocbase);

    res =
        Collections::create(vocbase,  // vocbase to create in
                            options,
                            name,  // collection name top create
                            TRI_COL_TYPE_DOCUMENT,  // collection type to create
                            bb.slice(),  // collection definition to create
                            true,        // waitsForSyncReplication
                            true,        // enforceReplicationFactor
                            isNewDatabase, createdCollection,
                            true /* allow system collection creation */);
  }

  TRI_ASSERT(res.fail() || createdCollection);
  return res;
}

Result Collections::properties(Context& ctxt, VPackBuilder& builder) {
  auto coll = ctxt.coll();
  TRI_ASSERT(coll != nullptr);
  ExecContext const& exec = ExecContext::current();
  bool canRead = exec.canUseCollection(coll->name(), auth::Level::RO);
  if (!canRead || exec.databaseAuthLevel() == auth::Level::NONE) {
    return Result(
        TRI_ERROR_FORBIDDEN,
        std::string("cannot access collection '") + coll->name() + "'");
  }

  std::unordered_set<std::string> ignoreKeys{StaticStrings::AllowUserKeys,
                                             StaticStrings::DataSourceCid,
                                             "count",
                                             StaticStrings::DataSourceDeleted,
                                             StaticStrings::DataSourceId,
                                             StaticStrings::Indexes,
                                             StaticStrings::DataSourceName,
                                             "path",
                                             StaticStrings::DataSourcePlanId,
                                             "shards",
                                             "status",
                                             StaticStrings::DataSourceType,
                                             StaticStrings::Version};

  if (ServerState::instance()->isSingleServer() &&
      (!coll->isSatellite() && !coll->isSmart())) {
    // 1. These are either relevant for cluster
    // 2. Or for collections which have additional cluster properties set for
    // future dump and restore use case. Currently those are supported during
    // graph creation. Therefore, we need those properties for satellite and
    // graph collections.
    ignoreKeys.insert(
        {StaticStrings::DistributeShardsLike, StaticStrings::IsSmart,
         StaticStrings::NumberOfShards, StaticStrings::ReplicationFactor,
         StaticStrings::MinReplicationFactor, StaticStrings::ShardKeys,
         StaticStrings::ShardingStrategy, StaticStrings::IsDisjoint});

    // this transaction is held longer than the following if...
    auto trx = ctxt.trx(AccessMode::Type::READ, true, false);
    TRI_ASSERT(trx != nullptr);
  }

  // note that we have an ongoing transaction here if we are in single-server
  // case
  VPackBuilder props = coll->toVelocyPackIgnore(
      ignoreKeys, LogicalDataSource::Serialization::Properties);
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackObjectIterator(props.slice()));

  return TRI_ERROR_NO_ERROR;
}

Result Collections::updateProperties(LogicalCollection& collection,
                                     velocypack::Slice props,
                                     OperationOptions const& options) {
  ExecContext const& exec = ExecContext::current();
  bool canModify = exec.canUseCollection(collection.name(), auth::Level::RW);

  if (!canModify || !exec.canUseDatabase(auth::Level::RW)) {
    return TRI_ERROR_FORBIDDEN;
  }

  if (ServerState::instance()->isCoordinator()) {
    ClusterInfo& ci = collection.vocbase()
                          .server()
                          .getFeature<ClusterFeature>()
                          .clusterInfo();
    auto info = ci.getCollection(collection.vocbase().name(),
                                 std::to_string(collection.id().id()));

    // replication checks
    if (auto s = props.get(StaticStrings::ReplicationFactor); s.isNumber()) {
      int64_t replFactor = Helper::getNumericValue<int64_t>(
          props, StaticStrings::ReplicationFactor, 0);
      if (replFactor > 0 &&
          static_cast<size_t>(replFactor) > ci.getCurrentDBServers().size()) {
        return TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS;
      }
    }

    // not an error: for historical reasons the write concern is read from the
    // variable "minReplicationFactor" if it exists
    uint64_t writeConcern =
        Helper::getNumericValue(props, StaticStrings::MinReplicationFactor, 0);
    if (props.hasKey(StaticStrings::WriteConcern)) {
      writeConcern =
          Helper::getNumericValue(props, StaticStrings::WriteConcern, 0);
    }

    // write-concern checks
    if (writeConcern > ci.getCurrentDBServers().size()) {
      TRI_ASSERT(writeConcern > 0);
      return TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS;
    }

    Result res = ShardingInfo::validateShardsAndReplicationFactor(
        props, collection.vocbase().server(), false);
    if (res.fail()) {
      return res;
    }

    auto rv = info->properties(props);
    if (rv.ok()) {
      velocypack::Builder builder(props);
      OperationResult result(rv, builder.steal(), options);
      events::PropertyUpdateCollection(collection.vocbase().name(),
                                       collection.name(), result);
    }
    return rv;

  } else {
    auto ctx =
        transaction::V8Context::CreateWhenRequired(collection.vocbase(), false);
    SingleCollectionTransaction trx(ctx, collection,
                                    AccessMode::Type::EXCLUSIVE);
    Result res = trx.begin();

    if (res.ok()) {
      // try to write new parameter to file
      res = collection.properties(props);
      if (res.ok()) {
        velocypack::Builder builder(props);
        OperationResult result(res, builder.steal(), options);
        events::PropertyUpdateCollection(collection.vocbase().name(),
                                         collection.name(), result);
      }
    }

    return res;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief helper function to rename collections in _graphs as well
////////////////////////////////////////////////////////////////////////////////

static ErrorCode RenameGraphCollections(TRI_vocbase_t& vocbase,
                                        std::string const& oldName,
                                        std::string const& newName) {
  ExecContextSuperuserScope exscope;

  graph::GraphManager gmngr{vocbase};
  bool r = gmngr.renameGraphCollection(oldName, newName);
  if (!r) {
    return TRI_ERROR_FAILED;
  }
  return TRI_ERROR_NO_ERROR;
}

Result Collections::rename(LogicalCollection& collection,
                           std::string const& newName, bool doOverride) {
  if (ServerState::instance()->isCoordinator()) {
    // renaming a collection in a cluster is unsupported
    return TRI_ERROR_CLUSTER_UNSUPPORTED;
  }

  if (newName.empty()) {
    return Result(TRI_ERROR_BAD_PARAMETER, "<name> must be non-empty");
  }

  ExecContext const& exec = ExecContext::current();
  if (!exec.canUseDatabase(auth::Level::RW) ||
      !exec.canUseCollection(collection.name(), auth::Level::RW)) {
    return TRI_ERROR_FORBIDDEN;
  }

  // check required to pass
  // shell-collection-rocksdb-noncluster.js::testSystemSpecial
  if (collection.system()) {
    return TRI_ERROR_FORBIDDEN;
  }

  if (!doOverride) {
    bool const isSystem = NameValidator::isSystemName(collection.name());

    if (isSystem != NameValidator::isSystemName(newName)) {
      // a system collection shall not be renamed to a non-system collection
      // name or vice versa
      return arangodb::Result(TRI_ERROR_ARANGO_ILLEGAL_NAME,
                              "a system collection shall not be renamed to a "
                              "non-system collection name or vice versa");
    }

    bool extendedNames = collection.vocbase()
                             .server()
                             .getFeature<DatabaseFeature>()
                             .extendedNamesForCollections();
    if (!CollectionNameValidator::isAllowedName(isSystem, extendedNames,
                                                newName)) {
      return TRI_ERROR_ARANGO_ILLEGAL_NAME;
    }
  }

  std::string const oldName(collection.name());
  auto res = collection.vocbase().renameCollection(collection.id(), newName);

  if (!res.ok()) {
    return res;
  }

  // rename collection inside _graphs as well
  return RenameGraphCollections(collection.vocbase(), oldName, newName);
}

#ifndef USE_ENTERPRISE

////////////////////////////////////////////////////////////////////////////////
/// @brief drops a collection, case of a coordinator in a cluster
////////////////////////////////////////////////////////////////////////////////

static Result DropVocbaseColCoordinator(arangodb::LogicalCollection* collection,
                                        bool allowDropSystem) {
  if (collection->system() && !allowDropSystem) {
    return TRI_ERROR_FORBIDDEN;
  }

  auto& databaseName = collection->vocbase().name();
  auto cid = std::to_string(collection->id().id());
  ClusterInfo& ci =
      collection->vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto res = ci.dropCollectionCoordinator(databaseName, cid, 300.0);

  if (!res.ok()) {
    return res;
  }

  collection->setStatus(TRI_VOC_COL_STATUS_DELETED);

  return TRI_ERROR_NO_ERROR;
}
#endif

/*static*/ arangodb::Result Collections::drop(  // drop collection
    arangodb::LogicalCollection& coll,          // collection to drop
    bool allowDropSystem,  // allow dropping system collection
    double timeout,        // single-server drop timeout
    bool keepUserRights) {
  ExecContext const& exec = ExecContext::current();
  if (!exec.canUseDatabase(coll.vocbase().name(),
                           auth::Level::RW) ||  // vocbase modifiable
      !exec.canUseCollection(coll.name(),
                             auth::Level::RW)) {  // collection modifiable
    events::DropCollection(coll.vocbase().name(), coll.name(),
                           TRI_ERROR_FORBIDDEN);
    return arangodb::Result(                                     // result
        TRI_ERROR_FORBIDDEN,                                     // code
        "Insufficient rights to drop collection " + coll.name()  // message
    );
  }

  auto const& dbname = coll.vocbase().name();
  std::string const collName = coll.name();
  Result res;

// If we are a coordinator in a cluster, we have to behave differently:
#ifdef USE_ENTERPRISE

  res = DropColEnterprise(&coll, allowDropSystem, timeout);
#else
  if (ServerState::instance()->isCoordinator()) {
    res = DropVocbaseColCoordinator(&coll, allowDropSystem);
  } else {
    res = coll.vocbase().dropCollection(coll.id(), allowDropSystem, timeout);
  }
#endif

  LOG_TOPIC_IF("1bf4d", WARN, Logger::ENGINES,
               res.fail() && res.isNot(TRI_ERROR_FORBIDDEN) &&
                   res.isNot(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) &&
                   res.isNot(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND))
      << "error while dropping collection: '" << collName << "' error: '"
      << res.errorMessage() << "'";

  if (ADB_LIKELY(!keepUserRights)) {
    auth::UserManager* um = AuthenticationFeature::instance()->userManager();

    if (res.ok() && um != nullptr) {
      res = um->enumerateUsers(
          [&](auth::User& entry) -> bool {
            return entry.removeCollection(dbname, collName);
          },
          /*retryOnConflict*/ true);
    }
  }
  events::DropCollection(coll.vocbase().name(), coll.name(), res.errorNumber());

  return res;
}

futures::Future<Result> Collections::warmup(TRI_vocbase_t& vocbase,
                                            LogicalCollection const& coll) {
  ExecContext const& exec = ExecContext::current();
  if (!exec.canUseCollection(coll.name(), auth::Level::RO)) {
    return futures::makeFuture(Result(TRI_ERROR_FORBIDDEN));
  }

  if (ServerState::instance()->isCoordinator()) {
    auto cid = std::to_string(coll.id().id());
    auto& feature = vocbase.server().getFeature<ClusterFeature>();
    OperationOptions options(exec);
    return warmupOnCoordinator(feature, vocbase.name(), cid, options);
  }

  StorageEngine& engine =
      vocbase.server().getFeature<EngineSelectorFeature>().engine();

  auto idxs = coll.getIndexes();
  for (auto const& idx : idxs) {
    if (idx->canWarmup()) {
      engine.scheduleFullIndexRefill(vocbase.name(), coll.name(), idx->id());
    }
  }
  return futures::makeFuture(Result());
}

futures::Future<OperationResult> Collections::revisionId(
    Context& ctxt, OperationOptions const& options) {
  if (ServerState::instance()->isCoordinator()) {
    auto& databaseName = ctxt.coll()->vocbase().name();
    auto cid = std::to_string(ctxt.coll()->id().id());
    auto& feature =
        ctxt.coll()->vocbase().server().getFeature<ClusterFeature>();
    return revisionOnCoordinator(feature, databaseName, cid, options);
  }

  RevisionId rid =
      ctxt.coll()->revision(ctxt.trx(AccessMode::Type::READ, true, true));

  VPackBuilder builder;
  builder.add(VPackValue(rid.toString()));

  return futures::makeFuture(
      OperationResult(Result(), builder.steal(), options));
}

arangodb::Result Collections::checksum(LogicalCollection& collection,
                                       bool withRevisions, bool withData,
                                       uint64_t& checksum, RevisionId& revId) {
  if (ServerState::instance()->isCoordinator()) {
    auto cid = std::to_string(collection.id().id());
    auto& feature = collection.vocbase().server().getFeature<ClusterFeature>();
    OperationOptions options(ExecContext::current());
    auto res = checksumOnCoordinator(feature, collection.vocbase().name(), cid,
                                     options, withRevisions, withData)
                   .get();
    if (res.ok()) {
      revId = RevisionId::fromSlice(res.slice().get("revision"));
      checksum = res.slice().get("checksum").getUInt();
    }
    return res.result;
  }

  ResourceMonitor monitor(GlobalResourceMonitor::instance());

  auto ctx =
      transaction::V8Context::CreateWhenRequired(collection.vocbase(), true);
  SingleCollectionTransaction trx(ctx, collection, AccessMode::Type::READ);
  Result res = trx.begin();

  if (res.fail()) {
    return res;
  }

  revId = collection.revision(&trx);
  checksum = 0;

  // We directly read the entire cursor. so batchsize == limit
  auto iterator =
      trx.indexScan(monitor, collection.name(),
                    transaction::Methods::CursorType::ALL, ReadOwnWrites::no);

  iterator->allDocuments([&](LocalDocumentId const& /*token*/,
                             VPackSlice slice) {
    uint64_t localHash =
        transaction::helpers::extractKeyFromDocument(slice).hashString();

    if (withRevisions) {
      localHash +=
          transaction::helpers::extractRevSliceFromDocument(slice).hash();
    }

    if (withData) {
      // with data
      uint64_t const n = slice.length() ^ 0xf00ba44ba5;
      uint64_t seed = fasthash64_uint64(n, 0xdeadf054);

      for (auto it : VPackObjectIterator(slice, false)) {
        // loop over all attributes, but exclude _rev, _id and _key
        // _id is different for each collection anyway, _rev is covered by
        // withRevisions, and _key was already handled before
        VPackValueLength keyLength;
        char const* key = it.key.getString(keyLength);
        if (keyLength >= 3 && key[0] == '_' &&
            ((keyLength == 3 && memcmp(key, "_id", 3) == 0) ||
             (keyLength == 4 &&
              (memcmp(key, "_key", 4) == 0 || memcmp(key, "_rev", 4) == 0)))) {
          // exclude attribute
          continue;
        }

        localHash ^= it.key.hash(seed) ^ 0xba5befd00d;
        localHash += it.value.normalizedHash(seed) ^ 0xd4129f526421;
      }
    }

    checksum ^= localHash;
    return true;
  });

  return trx.finish(res);
}

/// @brief the list of collection attributes that are allowed by user-input
/// this is to avoid retyping the same list twice
#define COMMON_ALLOWED_COLLECTION_INPUT_ATTRIBUTES                             \
  StaticStrings::DataSourceSystem, StaticStrings::DataSourceId,                \
      StaticStrings::KeyOptions, StaticStrings::WaitForSyncString,             \
      StaticStrings::CacheEnabled, StaticStrings::ShardKeys,                   \
      StaticStrings::NumberOfShards, StaticStrings::DistributeShardsLike,      \
      "avoidServers", StaticStrings::IsSmart, StaticStrings::ShardingStrategy, \
      StaticStrings::GraphSmartGraphAttribute, StaticStrings::Schema,          \
      StaticStrings::SmartJoinAttribute, StaticStrings::ReplicationFactor,     \
      StaticStrings::MinReplicationFactor, /* deprecated */                    \
      StaticStrings::WriteConcern, "servers", StaticStrings::ComputedValues

arangodb::velocypack::Builder Collections::filterInput(
    arangodb::velocypack::Slice properties, bool allowDC2DCAttributes) {
#ifdef USE_ENTERPRISE
  if (allowDC2DCAttributes) {
    return velocypack::Collection::keep(
        properties,
        std::unordered_set<std::string>{
            StaticStrings::IsDisjoint, StaticStrings::InternalValidatorTypes,
            COMMON_ALLOWED_COLLECTION_INPUT_ATTRIBUTES});
  }
#endif
  return velocypack::Collection::keep(
      properties, std::unordered_set<std::string>{
                      COMMON_ALLOWED_COLLECTION_INPUT_ATTRIBUTES});
}
