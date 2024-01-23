////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2023-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "Cluster/ClusterInfo.h"

#include "Agency/AgencyPaths.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/RecursiveLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/system-functions.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/AgencyCallback.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterHelpers.h"
#include "Cluster/ClusterCollectionCreationInfo.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "Logger/LogMacros.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"
#include "Utils/Events.h"
#include "VocBase/LogicalCollection.h"
#include "Replication2/Methods.h"
#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLeaderState.h"

using namespace arangodb;
using namespace arangodb::cluster;

namespace {
arangodb::AgencyOperation IncreaseVersion() {
  return arangodb::AgencyOperation{
      "Plan/Version", arangodb::AgencySimpleOperationType::INCREMENT_OP};
}

std::string collectionPath(std::string_view dbName,
                           std::string_view collection) {
  return "Plan/Collections/" + std::string{dbName} + "/" +
         std::string{collection};
}

arangodb::AgencyOperation CreateCollectionOrder(std::string_view dbName,
                                                std::string_view collection,
                                                VPackSlice info) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (!info.get("shards").isEmptyObject() &&
      !arangodb::basics::VelocyPackHelper::getBooleanValue(
          info, arangodb::StaticStrings::IsSmart, false)) {
    TRI_ASSERT(info.hasKey(arangodb::StaticStrings::AttrIsBuilding));
    TRI_ASSERT(info.get(arangodb::StaticStrings::AttrIsBuilding).isBool());
    TRI_ASSERT(info.get(arangodb::StaticStrings::AttrIsBuilding).getBool() ==
               true);
  }
#endif
  return arangodb::AgencyOperation{collectionPath(dbName, collection),
                                   arangodb::AgencyValueOperationType::SET,
                                   info};
}

arangodb::AgencyPrecondition CreateCollectionOrderPrecondition(
    std::string_view dbName, std::string_view collection, VPackSlice info) {
  return arangodb::AgencyPrecondition{collectionPath(dbName, collection),
                                      arangodb::AgencyPrecondition::Type::VALUE,
                                      info};
}

arangodb::AgencyOperation CreateCollectionSuccess(std::string_view dbName,
                                                  std::string_view collection,
                                                  VPackSlice info) {
  TRI_ASSERT(!info.hasKey(arangodb::StaticStrings::AttrIsBuilding));
  return arangodb::AgencyOperation{collectionPath(dbName, collection),
                                   arangodb::AgencyValueOperationType::SET,
                                   info};
}

auto createDocumentStateSpec(ShardID const& shardId,
                             std::vector<std::string> const& serverIds,
                             ClusterCollectionCreationInfo const& info,
                             std::string const& databaseName)
    -> replication2::agency::LogTarget {
  using namespace replication2::replicated_state;

  replication2::agency::LogTarget spec;

  // TODO remove this once we have the group ID
  spec.id = LogicalCollection::shardIdToStateId(shardId);

  spec.properties.implementation.type = document::DocumentState::NAME;
  auto parameters = document::DocumentCoreParameters{databaseName, 0, 0};
  spec.properties.implementation.parameters = parameters.toSharedSlice();

  TRI_ASSERT(!serverIds.empty());
  spec.leader = serverIds.front();

  for (auto const& serverId : serverIds) {
    spec.participants.emplace(serverId, replication2::ParticipantFlags{});
  }

  spec.config.writeConcern = info.writeConcern;
  spec.config.softWriteConcern = info.replicationFactor;
  spec.config.waitForSync = false;
  spec.version = 1;

  return spec;
}

}  // namespace

/// @brief this method does an atomic check of the preconditions for the
/// collections to be created, using the currently loaded plan.
Result ClusterInfo::checkCollectionPreconditions(
    std::string const& databaseName,
    std::vector<ClusterCollectionCreationInfo> const& infos) {
  for (auto const& info : infos) {
    // Check if name exists.
    if (info.name.empty() || !info.json.isObject() ||
        !info.json.get("shards").isObject()) {
      return TRI_ERROR_BAD_PARAMETER;  // must not be empty
    }

    // Validate that the collection does not exist in the current plan
    {
      AllCollections::const_iterator it =
          _plannedCollections.find(databaseName);
      if (it != _plannedCollections.end()) {
        DatabaseCollections::const_iterator it2 = (*it).second->find(info.name);

        if (it2 != (*it).second->end()) {
          // collection already exists!
          events::CreateCollection(databaseName, info.name,
                                   TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(
              TRI_ERROR_ARANGO_DUPLICATE_NAME,
              std::string("duplicate collection name '") + info.name + "'");
        }
      } else {
        // no collection in plan for this particular database... this may be
        // true for the first collection created in a db now check if there is a
        // planned database at least
        if (_plannedDatabases.find(databaseName) == _plannedDatabases.end()) {
          // no need to create a collection in a database that is not there
          // (anymore)
          events::CreateCollection(databaseName, info.name,
                                   TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
          return TRI_ERROR_ARANGO_DATABASE_NOT_FOUND;
        }
      }
    }

    // Validate that there is no view with this name either
    {
      // check against planned views as well
      auto it = _plannedViews.find(databaseName);
      if (it != _plannedViews.end()) {
        auto it2 = it->second.find(info.name);

        if (it2 != it->second.end()) {
          // view already exists!
          events::CreateCollection(databaseName, info.name,
                                   TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(
              TRI_ERROR_ARANGO_DUPLICATE_NAME,
              std::string("duplicate collection name '") + info.name + "'");
        }
      }
    }
  }

  return {};
}

auto ClusterInfo::deleteReplicatedStates(
    std::string const& databaseName,
    std::vector<replication2::LogId> const& replicatedStatesIds)
    -> futures::Future<Result> {
  auto replicatedStateMethods =
      arangodb::replication2::ReplicatedLogMethods::createInstance(databaseName,
                                                                   _server);

  std::vector<futures::Future<Result>> deletedStates;
  deletedStates.reserve(replicatedStatesIds.size());
  for (auto const& id : replicatedStatesIds) {
    deletedStates.emplace_back(replicatedStateMethods->deleteReplicatedLog(id));
  }

  return futures::collectAll(std::move(deletedStates))
      .then([](futures::Try<std::vector<futures::Try<Result>>>&& tryResult) {
        auto deletionResults =
            basics::catchToResultT([&] { return std::move(tryResult.get()); });

        auto makeResult = [](auto&& result) {
          Result r = result.mapError([](result::Error error) {
            error.appendErrorMessage(
                "Failed to delete replicated states corresponding to shards!");
            return error;
          });
          return r;
        };

        auto result = deletionResults.result();
        if (result.fail()) {
          return makeResult(std::move(result));
        }
        for (auto shardResult : deletionResults.get()) {
          auto r = basics::catchToResult(
              [&] { return std::move(shardResult.get()); });
          if (r.fail()) {
            return makeResult(std::move(r));
          }
        }

        return result;
      });
}

auto ClusterInfo::waitForReplicatedStatesCreation(
    std::string const& databaseName,
    std::vector<replication2::agency::LogTarget> const& replicatedStates)
    -> futures::Future<Result> {
  auto replicatedStateMethods =
      arangodb::replication2::ReplicatedLogMethods::createInstance(databaseName,
                                                                   _server);

  std::vector<futures::Future<ResultT<consensus::index_t>>> futureStates;
  futureStates.reserve(replicatedStates.size());
  for (auto const& spec : replicatedStates) {
    futureStates.emplace_back(
        replicatedStateMethods->waitForLogReady(spec.id, *spec.version));
  }

  // we need to define this here instead of using an inline lambda expression
  // because MSVC is too stupid to compile it otherwise
  auto appendErrorMessage = [](result::Error error) {
    error.appendErrorMessage(
        "Failed to create a corresponding replicated state "
        "for each shard!");
    return error;
  };

  return futures::collectAll(std::move(futureStates))
      .thenValue(
          [&clusterInfo = _server.getFeature<ClusterFeature>().clusterInfo()](
              auto&& raftIndices) {
            consensus::index_t maxIndex = 0;
            for (auto& v : raftIndices) {
              maxIndex = std::max(maxIndex, v.get().get());
            }
            return clusterInfo.fetchAndWaitForPlanVersion(
                std::chrono::seconds{240});
          })
      .then([&appendErrorMessage](auto&& tryResult) {
        Result result =
            basics::catchToResult([&] { return std::move(tryResult.get()); });
        if (result.fail()) {
          if (result.is(TRI_ERROR_NO_ERROR)) {
            result = Result(TRI_ERROR_INTERNAL, result.errorMessage());
          }
          result = result.mapError(appendErrorMessage);
        }
        return result;
      });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create collection in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly. One possible error
/// is a timeout, a timeout of 0.0 means no timeout.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::createCollectionCoordinator(  // create collection
    std::string const& databaseName, std::string const& collectionID,
    uint64_t numberOfShards, uint64_t replicationFactor, uint64_t writeConcern,
    bool waitForReplication,
    velocypack::Slice json,  // collection definition
    double timeout,          // request timeout,
    bool isNewDatabase,
    std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike,
    replication::Version replicationVersion) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  auto serverState = ServerState::instance();
  std::vector<ClusterCollectionCreationInfo> infos{
      ClusterCollectionCreationInfo{
          collectionID, numberOfShards, replicationFactor, writeConcern,
          waitForReplication, json, serverState->getId(),
          serverState->getRebootId()}};
  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  return createCollectionsCoordinator(databaseName, infos, endTime,
                                      isNewDatabase, colToDistributeShardsLike,
                                      replicationVersion);
}

Result ClusterInfo::createCollectionsCoordinator(
    std::string const& databaseName,
    std::vector<ClusterCollectionCreationInfo>& infos, double endTime,
    bool isNewDatabase,
    std::shared_ptr<const LogicalCollection> const& colToDistributeShardsLike,
    replication::Version replicationVersion) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  using arangodb::velocypack::Slice;

  LOG_TOPIC("98761", DEBUG, Logger::CLUSTER)
      << "Starting createCollectionsCoordinator for " << infos.size()
      << " collections in database " << databaseName
      << " isNewDatabase: " << isNewDatabase
      << " first collection name: " << infos[0].name;

  // The following three are used for synchronization between the callback
  // closure and the main thread executing this function. Note that it can
  // happen that the callback is called only after we return from this
  // function!
  auto dbServerResult =
      std::make_shared<std::atomic<std::optional<ErrorCode>>>(std::nullopt);
  auto nrDone = std::make_shared<std::atomic<size_t>>(0);
  auto errMsg = std::make_shared<std::string>();
  auto cacheMutex = std::make_shared<std::recursive_mutex>();
  auto isCleaned = std::make_shared<bool>(false);

  AgencyComm ac(_server);
  std::vector<std::shared_ptr<AgencyCallback>> agencyCallbacks;

  auto cbGuard = scopeGuard([&]() noexcept {
    try {
      // We have a subtle race here, that we try to cover against:
      // We register a callback in the agency.
      // For some reason this scopeguard is executed (e.g. error case)
      // While we are in this cleanup, and before a callback is removed from the
      // agency. The callback is triggered by another thread. We have the
      // following guarantees: a) cacheMutex|Owner are valid and locked by
      // cleanup b) isCleaned is valid and now set to true c) the closure is
      // owned by the callback d) info might be deleted, so we cannot use it. e)
      // If the callback is ongoing during cleanup, the callback will
      //    hold the Mutex and delay the cleanup.
      std::lock_guard lock{*cacheMutex};
      *isCleaned = true;
      for (auto& cb : agencyCallbacks) {
        _agencyCallbackRegistry.unregisterCallback(cb);
      }
    } catch (std::exception const&) {
    }
  });

  std::vector<AgencyOperation> opers({IncreaseVersion()});
  std::vector<AgencyPrecondition> precs;
  containers::FlatHashSet<std::string> conditions;
  containers::FlatHashSet<ServerID> allServers;
  std::vector<replication2::agency::LogTarget> replicatedStates;

  // current thread owning 'cacheMutex' write lock (workaround for non-recursive
  // Mutex)
  for (auto& info : infos) {
    TRI_IF_FAILURE("ClusterInfo::requiresWaitForReplication") {
      if (info.waitForReplication) {
        return TRI_ERROR_DEBUG;
      } else {
        TRI_ASSERT(false) << "We required to have waitForReplication, but it "
                             "was set to false";
      }
    }
    TRI_ASSERT(!info.name.empty());

    if (info.state == ClusterCollectionCreationState::DONE) {
      // This is possible in Enterprise / Smart Collection situation
      (*nrDone)++;
    }

    std::map<ShardID, std::vector<ServerID>> shardServers;
    for (auto pair : VPackObjectIterator(info.json.get("shards"))) {
      ShardID shardID{pair.key.copyString()};
      std::vector<ServerID> serverIds;

      for (auto const& serv : VPackArrayIterator(pair.value)) {
        auto const sid = serv.copyString();
        serverIds.emplace_back(sid);
        allServers.emplace(sid);
      }
      shardServers.try_emplace(std::move(shardID), std::move(serverIds));
    }

    // Counts the elements of result in nrDone and checks that they match
    // shardServers. Also checks that result matches info. Errors are stored in
    // the database via dbServerResult, in errMsg and in info.state.
    //
    // The AgencyCallback will copy the closure and take responsibility of it.
    // this here is ok as ClusterInfo is not destroyed
    // for &info it should have ensured lifetime somehow. OR ensured that
    // callback is noop in case it is triggered too late.
    auto closure = [cacheMutex, &info, dbServerResult, errMsg, nrDone,
                    isCleaned, shardServers, this](VPackSlice result) {
      // NOTE: This ordering here is important to cover against a race in
      // cleanup. a) The Guard get's the Mutex, sets isCleaned == true, then
      // removes the callback b) If the callback is acquired it is saved in a
      // shared_ptr, the Mutex will be acquired first, then it will check if it
      // isCleaned
      std::lock_guard lock{*cacheMutex};
      if (*isCleaned) {
        return true;
      }
      TRI_ASSERT(!info.name.empty());
      if (info.state != ClusterCollectionCreationState::INIT) {
        // All leaders have reported either good or bad
        // We might be called by followers if they get in sync fast enough
        // In this IF we are in the followers case, we can safely ignore
        return true;
      }

      // result is the object at the path
      if (result.isObject() &&
          result.length() == static_cast<size_t>(info.numberOfShards)) {
        std::string tmpError;

        for (auto const& p : VPackObjectIterator(result)) {
          // if p contains an error number, add it to tmpError as a string
          if (arangodb::basics::VelocyPackHelper::getBooleanValue(
                  p.value, StaticStrings::Error, false)) {
            tmpError += " shardID:" + p.key.copyString() + ":";
            tmpError += arangodb::basics::VelocyPackHelper::getStringValue(
                p.value, StaticStrings::ErrorMessage, "");
            if (p.value.hasKey(StaticStrings::ErrorNum)) {
              VPackSlice const errorNum = p.value.get(StaticStrings::ErrorNum);
              if (errorNum.isNumber()) {
                tmpError += " (errNum=";
                tmpError += basics::StringUtils::itoa(
                    errorNum.getNumericValue<uint32_t>());
                tmpError += ")";
              }
            }
          }

          // wait that all followers have created our new collection
          if (tmpError.empty() && info.waitForReplication) {
            std::vector<ServerID> plannedServers;
            // copy all servers which are in p from shardServers to
            // plannedServers
            {
              READ_LOCKER(readLocker, _planProt.lock);
              auto it = shardServers.find(ShardID{p.key.copyString()});
              if (it != shardServers.end()) {
                plannedServers = (*it).second;
              } else {
                LOG_TOPIC("9ed54", ERR, Logger::CLUSTER)
                    << "Did not find shard in _shardServers: "
                    << p.key.copyString()
                    << ". Maybe the collection is already dropped.";
                *errMsg =
                    "Error in creation of collection: " + p.key.copyString() +
                    ". Collection already dropped. " + __FILE__ + ":" +
                    std::to_string(__LINE__);
                dbServerResult->store(
                    TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION,
                    std::memory_order_release);
                TRI_ASSERT(info.state != ClusterCollectionCreationState::DONE);
                info.state = ClusterCollectionCreationState::FAILED;
                return true;
              }
            }
            if (plannedServers.empty()) {
              READ_LOCKER(readLocker, _planProt.lock);
              LOG_TOPIC("a0a76", DEBUG, Logger::CLUSTER)
                  << "This should never have happened, Plan empty. Dumping "
                     "_shards in Plan:";
              for (auto const& s : _shards) {
                LOG_TOPIC("60c7d", DEBUG, Logger::CLUSTER)
                    << "Shard: " << s.first;
                for (auto const& q : *(s.second)) {
                  LOG_TOPIC("c7363", DEBUG, Logger::CLUSTER)
                      << "  Server: " << q;
                }
              }
              TRI_ASSERT(false);
            }
            std::vector<ServerID> currentServers;
            VPackSlice servers = p.value.get("servers");
            if (!servers.isArray()) {
              return true;
            }
            for (auto const& server : VPackArrayIterator(servers)) {
              if (!server.isString()) {
                return true;
              }
              currentServers.push_back(server.copyString());
            }
            if (!ClusterHelpers::compareServerLists(plannedServers,
                                                    currentServers)) {
              TRI_ASSERT(!info.name.empty());
              LOG_TOPIC("16623", DEBUG, Logger::CLUSTER)
                  << "Still waiting for all servers to ACK creation of "
                  << info.name << ". Planned: " << plannedServers
                  << ", Current: " << currentServers;
              return true;
            }
          }
        }
        if (!tmpError.empty()) {
          *errMsg = "Error in creation of collection:" + tmpError + " " +
                    __FILE__ + std::to_string(__LINE__);
          dbServerResult->store(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION,
                                std::memory_order_release);
          // We cannot get into bad state after a collection was created
          TRI_ASSERT(info.state != ClusterCollectionCreationState::DONE);
          info.state = ClusterCollectionCreationState::FAILED;
        } else {
          // We can have multiple calls to this callback, one per leader and one
          // per follower As soon as all leaders are done we are either FAILED
          // or DONE, this cannot be altered later.
          TRI_ASSERT(info.state != ClusterCollectionCreationState::FAILED);
          info.state = ClusterCollectionCreationState::DONE;
          (*nrDone)++;
        }
      }
      return true;
    };  // closure

    // ATTENTION: The following callback calls the above closure in a
    // different thread. Nevertheless, the closure accesses some of our
    // local variables. Therefore, we have to protect all accesses to them
    // by a mutex. We use the mutex of the condition variable in the
    // AgencyCallback for this.

    auto agencyCallback = std::make_shared<AgencyCallback>(
        _server,
        "Current/Collections/" + databaseName + "/" + info.collectionID,
        closure, true, false);

    Result r = _agencyCallbackRegistry.registerCallback(agencyCallback);
    if (r.fail()) {
      return r;
    }

    agencyCallbacks.emplace_back(std::move(agencyCallback));
    opers.emplace_back(CreateCollectionOrder(databaseName, info.collectionID,
                                             info.isBuildingSlice()));

    if (replicationVersion == replication::Version::TWO) {
      // Create a replicated state for each shard.
      replicatedStates.reserve(replicatedStates.size() + shardServers.size());
      for (auto const& [shardId, serverIds] : shardServers) {
        auto spec =
            createDocumentStateSpec(shardId, serverIds, info, databaseName);

        auto builder = std::make_shared<VPackBuilder>();
        velocypack::serialize(*builder, spec);
        auto path = paths::aliases::target()
                        ->replicatedLogs()
                        ->database(databaseName)
                        ->log(spec.id);

        opers.emplace_back(AgencyOperation(path, AgencyValueOperationType::SET,
                                           std::move(builder)));
        replicatedStates.emplace_back(std::move(spec));
      }
    }

    // Ensure preconditions on the agency
    std::shared_ptr<ShardMap> otherCidShardMap = nullptr;
    auto const otherCidString = basics::VelocyPackHelper::getStringValue(
        info.json, StaticStrings::DistributeShardsLike, StaticStrings::Empty);
    if (!otherCidString.empty() &&
        conditions.find(otherCidString) == conditions.end()) {
      // Distribute shards like case.
      // Precondition: Master collection is not moving while we create this
      // collection We only need to add these once for every Master, we cannot
      // add multiple because we will end up with duplicate entries.
      // NOTE: We do not need to add all collections created here, as they will
      // not succeed In callbacks if they are moved during creation. If they are
      // moved after creation was reported success they are under protection by
      // Supervision.
      conditions.emplace(otherCidString);
      if (colToDistributeShardsLike != nullptr) {
        otherCidShardMap = colToDistributeShardsLike->shardIds();
      } else {
        otherCidShardMap =
            getCollection(databaseName, otherCidString)->shardIds();
      }

      auto const dslProtoColPath = paths::root()
                                       ->arango()
                                       ->plan()
                                       ->collections()
                                       ->database(databaseName)
                                       ->collection(otherCidString);
      // The distributeShardsLike prototype collection should exist in the
      // plan...
      precs.emplace_back(AgencyPrecondition(
          dslProtoColPath, AgencyPrecondition::Type::EMPTY, false));
      // ...and should not still be in creation.
      precs.emplace_back(AgencyPrecondition(dslProtoColPath->isBuilding(),
                                            AgencyPrecondition::Type::EMPTY,
                                            true));

      // Any of the shards locked?
      for (auto const& shard : *otherCidShardMap) {
        precs.emplace_back(
            AgencyPrecondition("Supervision/Shards/" + shard.first,
                               AgencyPrecondition::Type::EMPTY, true));
      }
    }

    // additionally ensure that no such collectionID exists yet in
    // Plan/Collections
    precs.emplace_back(AgencyPrecondition(
        "Plan/Collections/" + databaseName + "/" + info.collectionID,
        AgencyPrecondition::Type::EMPTY, true));
  }

  // We need to make sure our plan is up-to-date.
  LOG_TOPIC("f4b14", DEBUG, Logger::CLUSTER)
      << "createCollectionCoordinator, loading Plan from agency...";

  uint64_t planVersion = 0;  // will be populated by following function call
  {
    READ_LOCKER(readLocker, _planProt.lock);
    planVersion = _planVersion;
    if (!isNewDatabase) {
      Result res = checkCollectionPreconditions(databaseName, infos);
      if (res.fail()) {
        LOG_TOPIC("98762", DEBUG, Logger::CLUSTER)
            << "Failed createCollectionsCoordinator for " << infos.size()
            << " collections in database " << databaseName
            << " isNewDatabase: " << isNewDatabase << " first collection name: "
            << (!infos.empty() ? infos[0].name : std::string());
        return res;
      }
    }
  }

  auto deleteCollectionGuard = scopeGuard([&infos, &databaseName, this, &ac,
                                           replicationVersion,
                                           &replicatedStates]() noexcept {
    try {
      using namespace arangodb::cluster::paths;
      using namespace arangodb::cluster::paths::aliases;
      // We need to check isBuilding as a precondition.
      // If the transaction removing the isBuilding flag results in a timeout,
      // the state of the collection is unknown; if it was actually removed, we
      // must not drop the collection, but we must otherwise.

      auto precs = std::vector<AgencyPrecondition>{};
      auto opers = std::vector<AgencyOperation>{};

      // Note that we trust here that either all isBuilding flags are removed in
      // a single transaction, or none is.

      for (auto const& info : infos) {
        using namespace std::string_literals;
        auto const collectionPlanPath =
            "Plan/Collections/"s + databaseName + "/" + info.collectionID;
        precs.emplace_back(
            collectionPlanPath + "/" + StaticStrings::AttrIsBuilding,
            AgencyPrecondition::Type::EMPTY, false);
        opers.emplace_back(collectionPlanPath,
                           AgencySimpleOperationType::DELETE_OP);
      }
      opers.emplace_back("Plan/Version",
                         AgencySimpleOperationType::INCREMENT_OP);
      auto trx = AgencyWriteTransaction{opers, precs};

      auto replicatedStatesCleanup = futures::Future<Result>{std::in_place};
      if (replicationVersion == replication::Version::TWO) {
        std::vector<replication2::LogId> stateIds;
        std::transform(replicatedStates.begin(), replicatedStates.end(),
                       std::back_inserter(stateIds),
                       [](replication2::agency::LogTarget const& spec) {
                         return spec.id;
                       });

        replicatedStatesCleanup =
            this->deleteReplicatedStates(databaseName, stateIds);
      }

      using namespace std::chrono;
      using namespace std::chrono_literals;
      auto const begin = steady_clock::now();
      // After a shutdown, the supervision will clean the collections either due
      // to the coordinator going into FAIL, or due to it changing its rebootId.
      // Otherwise we must under no circumstance give up here, because noone
      // else will clean this up.
      while (!_server.isStopping()) {
        auto res = ac.sendTransactionWithFailover(trx);
        // If the collections were removed (res.ok()), we may abort. If we run
        // into precondition failed, the collections were successfully created,
        // so we're fine too.
        if (res.successful() &&
            (replicationVersion == replication::Version::ONE ||
             replicatedStatesCleanup.isReady())) {
          if (VPackSlice resultsSlice = res.slice().get("results");
              resultsSlice.length() > 0) {
            [[maybe_unused]] Result r =
                waitForPlan(resultsSlice[0].getNumber<uint64_t>()).get();
          }
          return;
        } else if (res.httpCode() == rest::ResponseCode::PRECONDITION_FAILED) {
          return;
        }

        // exponential backoff, just to be safe,
        auto const durationSinceStart = steady_clock::now() - begin;
        auto constexpr maxWaitTime = 2min;
        auto const waitTime =
            std::min<std::common_type_t<decltype(durationSinceStart),
                                        decltype(maxWaitTime)>>(
                durationSinceStart, maxWaitTime);
        std::this_thread::sleep_for(waitTime);
      }

    } catch (std::exception const&) {
    }
  });

  // now try to update the plan in the agency, using the current plan version as
  // our precondition
  {
    // create a builder with just the version number for comparison
    VPackBuilder versionBuilder;
    versionBuilder.add(VPackValue(planVersion));

    VPackBuilder serversBuilder;
    {
      VPackArrayBuilder a(&serversBuilder);
      for (auto const& i : allServers) {
        serversBuilder.add(VPackValue(i));
      }
    }

    // Preconditions:
    // * plan version unchanged
    precs.emplace_back(AgencyPrecondition("Plan/Version",
                                          AgencyPrecondition::Type::VALUE,
                                          versionBuilder.slice()));
    // * not in to be cleaned server list
    precs.emplace_back(AgencyPrecondition(
        "Target/ToBeCleanedServers",
        AgencyPrecondition::Type::INTERSECTION_EMPTY, serversBuilder.slice()));
    // * not in cleaned server list
    precs.emplace_back(AgencyPrecondition(
        "Target/CleanedServers", AgencyPrecondition::Type::INTERSECTION_EMPTY,
        serversBuilder.slice()));

    AgencyWriteTransaction transaction(opers, precs);

    {  // we hold this mutex from now on until we have updated our cache
      // using loadPlan, this is necessary for the callback closure to
      // see the new planned state for this collection. Otherwise it cannot
      // recognize completion of the create collection operation properly:
      std::lock_guard lock{*cacheMutex};
      auto res = ac.sendTransactionWithFailover(transaction);
      // Only if not precondition failed
      if (!res.successful()) {
        if (res.httpCode() == rest::ResponseCode::PRECONDITION_FAILED) {
          // use this special error code to signal that we got a precondition
          // failure in this case the caller can try again with an updated
          // version of the plan change
          LOG_TOPIC("98763", DEBUG, Logger::CLUSTER)
              << "Failed createCollectionsCoordinator for " << infos.size()
              << " collections in database " << databaseName
              << " isNewDatabase: " << isNewDatabase
              << " first collection name: " << infos[0].name;
          return {TRI_ERROR_CLUSTER_CREATE_COLLECTION_PRECONDITION_FAILED,
                  "operation aborted due to precondition failure"};
        }
        auto errorMsg = basics::StringUtils::concatT(
            "HTTP code: ", static_cast<int>(res.httpCode()),
            " error message: ", res.errorMessage(),
            " error details: ", res.errorDetails(), " body: ", res.body());
        for (auto const& info : infos) {
          events::CreateCollection(
              databaseName, info.name,
              TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION_IN_PLAN);
        }
        LOG_TOPIC("98767", DEBUG, Logger::CLUSTER)
            << "Failed createCollectionsCoordinator for " << infos.size()
            << " collections in database " << databaseName
            << " isNewDatabase: " << isNewDatabase << " first collection name: "
            << (!infos.empty() ? infos[0].name : std::string());
        return {TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION_IN_PLAN,
                std::move(errorMsg)};
      }

      if (VPackSlice resultsSlice = res.slice().get("results");
          resultsSlice.length() > 0) {
        Result r = waitForPlan(resultsSlice[0].getNumber<uint64_t>()).get();
        if (r.fail()) {
          return r;
        }
      }
    }
  }

  TRI_IF_FAILURE("ClusterInfo::createCollectionsCoordinator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  auto replicatedStatesWait = std::invoke([&]() -> futures::Future<Result> {
    if (replicationVersion == replication::Version::TWO) {
      // TODO could the version of a replicated state change in the meantime?
      return waitForReplicatedStatesCreation(databaseName, replicatedStates);
    }
    return Result{};
  });

  do {
    auto tmpRes = dbServerResult->load(std::memory_order_acquire);
    if (TRI_microtime() > endTime) {
      for (auto const& info : infos) {
        LOG_TOPIC("f6b57", ERR, Logger::CLUSTER)
            << "Timeout in _create collection"
            << ": database: " << databaseName
            << ", collId:" << info.collectionID
            << "\njson: " << info.json.toString();
      }

      if (replicationVersion == replication::Version::TWO) {
        LOG_TOPIC("6d279", ERR, Logger::REPLICATION2)
            << "Replicated states readiness: " << std::boolalpha
            << replicatedStatesWait.isReady();
      }

      // Get a full agency dump for debugging
      logAgencyDump();

      if (!tmpRes.has_value() || *tmpRes == TRI_ERROR_NO_ERROR) {
        tmpRes = TRI_ERROR_CLUSTER_TIMEOUT;
      }
    }

    if (nrDone->load(std::memory_order_acquire) == infos.size() &&
        (replicationVersion == replication::Version::ONE ||
         replicatedStatesWait.isReady())) {
      if (replicationVersion == replication::Version::TWO) {
        auto result = replicatedStatesWait.get();
        if (result.fail()) {
          LOG_TOPIC("ce2be", WARN, Logger::CLUSTER)
              << "Failed createCollectionsCoordinator for " << infos.size()
              << " collections in database " << databaseName
              << " isNewDatabase: " << isNewDatabase
              << " first collection name: " << infos[0].name
              << " result: " << result;
          return result;
        }
      }

      // We do not need to lock all condition variables
      // we are save by cacheMutex
      cbGuard.fire();
      // Now we need to remove the AttrIsBuilding flag and the creator in the
      // Agency
      opers.clear();
      precs.clear();
      opers.push_back(IncreaseVersion());
      for (auto const& info : infos) {
        opers.emplace_back(CreateCollectionSuccess(
            databaseName, info.collectionID, info.json));
        // NOTE:
        // We cannot do anything better than: "noone" has modified our
        // collections while we tried to create them... Preconditions cover
        // against supervision jobs injecting other leaders / followers during
        // failovers. If they are it is not valid to confirm them here. (bad
        // luck we were almost there)
        precs.emplace_back(CreateCollectionOrderPrecondition(
            databaseName, info.collectionID, info.isBuildingSlice()));
      }

      AgencyWriteTransaction transaction(opers, precs);

      // This is a best effort, in the worst case the collection stays, but will
      // be cleaned out by deleteCollectionGuard respectively the supervision.
      // This removes *all* isBuilding flags from all collections. This is
      // important so that the creation of all collections is atomic, and
      // the deleteCollectionGuard relies on it, too.
      auto res = ac.sendTransactionWithFailover(transaction);

      TRI_IF_FAILURE(
          "ClusterInfo::createCollectionsCoordinatorRemoveIsBuilding") {
        res.set(rest::ResponseCode::PRECONDITION_FAILED,
                "Failed to mark collection ready");
      }

      if (res.successful()) {
        // Note that this is not strictly necessary, just to avoid an
        // unneccessary request when we're sure that we don't need it anymore.
        deleteCollectionGuard.cancel();
        if (VPackSlice resultsSlice = res.slice().get("results");
            resultsSlice.length() > 0) {
          Result r = waitForPlan(resultsSlice[0].getNumber<uint64_t>()).get();
          if (r.fail()) {
            return r;
          }
        }
      } else {
        return Result(TRI_ERROR_HTTP_SERVICE_UNAVAILABLE,
                      "A cluster backend which was required for the operation "
                      "could not be reached");
      }

      // Report if this operation worked, if it failed collections will be
      // cleaned up by deleteCollectionGuard.
      for (auto const& info : infos) {
        TRI_ASSERT(info.state == ClusterCollectionCreationState::DONE);
        events::CreateCollection(databaseName, info.name, res.errorCode());
      }
      return res.asResult();
    }
    if (tmpRes.has_value() && tmpRes != TRI_ERROR_NO_ERROR) {
      // We do not need to lock all condition variables
      // we are safe by using cacheMutex
      cbGuard.fire();

      // report error
      for (auto const& info : infos) {
        // Report first error.
        // On timeout report it on all not finished ones.
        if (info.state == ClusterCollectionCreationState::FAILED ||
            (tmpRes == TRI_ERROR_CLUSTER_TIMEOUT &&
             info.state == ClusterCollectionCreationState::INIT)) {
          events::CreateCollection(databaseName, info.name, *tmpRes);
        }
      }
      LOG_TOPIC("98765", DEBUG, Logger::CLUSTER)
          << "Failed createCollectionsCoordinator for " << infos.size()
          << " collections in database " << databaseName
          << " isNewDatabase: " << isNewDatabase
          << " first collection name: " << infos[0].name
          << " result: " << *tmpRes;
      return {*tmpRes, *errMsg};
    }

    // If we get here we have not tried anything.
    // Wait on callbacks.

    if (_server.isStopping()) {
      // Report shutdown on all collections
      for (auto const& info : infos) {
        events::CreateCollection(databaseName, info.name,
                                 TRI_ERROR_SHUTTING_DOWN);
      }
      return TRI_ERROR_SHUTTING_DOWN;
    }

    // Wait for Callbacks to be triggered, it is sufficient to wait for the
    // first non, done
    TRI_ASSERT(agencyCallbacks.size() == infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
      if (infos[i].state == ClusterCollectionCreationState::INIT) {
        bool gotTimeout;
        {
          // This one has not responded, wait for it.
          std::lock_guard locker{agencyCallbacks[i]->_cv.mutex};
          gotTimeout =
              agencyCallbacks[i]->executeByCallbackOrTimeout(getPollInterval());
        }
        if (gotTimeout) {
          ++i;
          // We got woken up by waittime, not by  callback.
          // Let us check if we skipped other callbacks as well
          for (; i < infos.size(); ++i) {
            if (infos[i].state == ClusterCollectionCreationState::INIT) {
              agencyCallbacks[i]->refetchAndUpdate(true, false);
            }
          }
        }
        break;
      }
    }

  } while (!_server.isStopping());
  // If we get here we are not allowed to retry.
  // The loop above does not contain a break
  TRI_ASSERT(_server.isStopping());
  for (auto const& info : infos) {
    events::CreateCollection(databaseName, info.name, TRI_ERROR_SHUTTING_DOWN);
  }
  return Result{TRI_ERROR_SHUTTING_DOWN};
}
