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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "RestAdminClusterHandler.h"

#include <chrono>

#include <boost/functional/hash.hpp>

#include <velocypack/Buffer.h>
#include <velocypack/Iterator.h>

#include "Agency/AgencyPaths.h"
#include "Agency/AsyncAgencyComm.h"
#include "Agency/Supervision.h"
#include "Agency/TransactionBuilder.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/NumberUtils.h"
#include "Basics/ResultT.h"
#include "Basics/StaticStrings.h"
#include "Basics/TimeString.h"
#include "Cluster/AutoRebalance.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/AgencyCallback.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterHelpers.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/ServerSecurityFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardDistributionReporter.h"
#include "StorageEngine/VPackSortMigration.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Databases.h"
#include "Inspection/VPack.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

using namespace std::chrono_literals;

namespace {

struct agentConfigHealthResult {
  std::string endpoint, name;
  futures::Try<network::Response> response;
};

void removePlanServersReplicationOne(std::unordered_set<std::string>& servers,
                                     VPackSlice planCollections) {
  for (auto const& collection : VPackObjectIterator(planCollections)) {
    // In ReplicationOne we need to ignore servers that is either leader or
    // follower of a shard.
    VPackSlice shards = collection.value.get("shards");
    for (auto const& shard : VPackObjectIterator(shards)) {
      for (auto const& server : VPackArrayIterator(shard.value)) {
        servers.erase(server.copyString());
        if (servers.empty()) {
          return;
        }
      }
    }
  }
}

void removeCurrentServersReplicationOne(
    std::unordered_set<std::string>& servers, VPackSlice currentCollections) {
  for (auto const& collection : VPackObjectIterator(currentCollections)) {
    for (auto const& shard : VPackObjectIterator(collection.value)) {
      for (auto const& server :
           VPackArrayIterator(shard.value.get("servers"))) {
        servers.erase(server.copyString());
        if (servers.empty()) {
          return;
        }
      }
    }
  }
}

void removePlanServersReplicationTwo(std::unordered_set<std::string>& servers,
                                     VPackSlice planReplicatedLogs) {
  for (auto const& logSlice : VPackObjectIterator(planReplicatedLogs)) {
    auto log = velocypack::deserialize<
        arangodb::replication2::agency::LogPlanSpecification>(logSlice.value);
    // In ReplicationTwo we need to ignore servers that are still leading the
    // logs
    if (log.currentTerm.has_value() &&
        log.currentTerm.value().leader.has_value()) {
      // If we have selected a leader, let's avoid to remove it
      // Should be cleaned out first
      servers.erase(log.currentTerm.value().leader.value().serverId);
    } else {
      // If we don't have a leader, let's not remove any server that is still a
      // participant
      for (auto const& [server, flags] : log.participantsConfig.participants) {
        servers.erase(server);
      }
    }
  }
}

void removeCurrentServersReplicationTwo(
    std::unordered_set<std::string>& servers,
    VPackSlice currentReplicatedLogs) {
  for (auto const& logSlice : VPackObjectIterator(currentReplicatedLogs)) {
    auto log =
        velocypack::deserialize<arangodb::replication2::agency::LogCurrent>(
            logSlice.value);
    if (log.leader.has_value()) {
      servers.erase(log.leader.value().serverId);
    }
  }
}

void removePlanOrCurrentServers(std::unordered_set<std::string>& servers,
                                VPackSlice plan, VPackSlice current) {
  for (auto const& database : VPackObjectIterator(plan.get("Databases"))) {
    auto replVersion = database.value.get(StaticStrings::ReplicationVersion);
    if (replVersion.isString() && replVersion.isEqualString("2")) {
      auto planLogs = plan.get("ReplicatedLogs").get(database.key.stringView());
      auto currentLogs =
          current.get("ReplicatedLogs").get(database.key.stringView());
      removePlanServersReplicationTwo(servers, planLogs);
      removeCurrentServersReplicationTwo(servers, currentLogs);
    } else {
      auto planCollections =
          plan.get("Collections").get(database.key.stringView());
      auto currentCollections =
          current.get("Collections").get(database.key.stringView());
      removePlanServersReplicationOne(servers, planCollections);
      removeCurrentServersReplicationOne(servers, currentCollections);
    }
  }
}

bool isServerResponsibleForSomething(std::string const& server, VPackSlice plan,
                                     VPackSlice current) {
  std::unordered_set<std::string> servers = {server};
  removePlanOrCurrentServers(servers, plan, current);
  return servers.size() == 1;
}

template<typename T, typename F>
struct delayedCalculator {
  bool constructed;
  T content;
  F constructor;

  delayedCalculator(delayedCalculator const&) = delete;
  delayedCalculator(delayedCalculator&&) noexcept = delete;
  template<typename U>
  explicit delayedCalculator(U&& c)
      : constructed(false), constructor(std::forward<U>(c)) {}

  T* operator->() {
    if (!constructed) {
      constructed = true;
      content = constructor();
    }
    return &content;
  }
};

template<typename F>
delayedCalculator(F) -> delayedCalculator<std::invoke_result_t<F>, F>;

void buildHealthResult(
    VPackBuilder& builder,
    std::vector<futures::Try<agentConfigHealthResult>> const& config,
    VPackSlice store) {
  auto rootPath = arangodb::cluster::paths::root()->arango();

  using server_set = std::unordered_set<std::string>;

  auto canBeDeletedConstructor = [&]() {
    server_set set;
    {
      VPackObjectIterator memberIter(
          store.get(rootPath->supervision()->health()->vec()));
      for (auto member : memberIter) {
        set.insert(member.key.copyString());
      }
    }

    removePlanOrCurrentServers(set, store.get(rootPath->plan()->vec()),
                               store.get(rootPath->current()->vec()));
    return set;
  };
  delayedCalculator canBeDeleted(canBeDeletedConstructor);

  struct AgentInformation {
    bool leader;
    double lastAcked;
    AgentInformation() : leader(false), lastAcked(0.0) {}
  };

  std::unordered_map<std::string, AgentInformation> agents;

  // gather information about the agents
  for (auto const& agentTry : config) {
    TRI_ASSERT(agentTry.hasValue());

    auto& agent = agentTry.get();
    // check if the agent responded. If not, ignore. This is just for building
    // up agent information.
    if (agent.response.hasValue()) {
      auto& response = agent.response.get();
      if (response.ok() && response.statusCode() == fuerte::StatusOK) {
        VPackSlice lastAcked = response.slice().get("lastAcked");
        if (lastAcked.isNone()) {
          continue;
        }
        agents[agent.name].leader = true;
        for (const auto& agentIter : VPackObjectIterator(lastAcked)) {
          agents[agentIter.key.copyString()].lastAcked =
              agentIter.value.get("lastAckedTime").getDouble();
        }
      }
    }
  }

  builder.add("ClusterId", store.get(rootPath->cluster()->vec()));
  {
    VPackObjectBuilder ob(&builder, "Health");

    VPackObjectIterator memberIter(
        store.get(rootPath->supervision()->health()->vec()));
    for (auto member : memberIter) {
      std::string serverId = member.key.copyString();

      {
        VPackObjectBuilder obMember(&builder, serverId);

        builder.add(VPackObjectIterator(member.value));
        if (ClusterHelpers::isDBServerName(serverId)) {
          builder.add("Role", VPackValue("DBServer"));
          builder.add("CanBeDeleted",
                      VPackValue(VPackValue(
                          member.value.get("Status").isEqualString("FAILED") &&
                          canBeDeleted->count(serverId) == 1)));
        } else if (ClusterHelpers::isCoordinatorName(serverId)) {
          builder.add("Role", VPackValue("Coordinator"));
          builder.add(
              "CanBeDeleted",
              VPackValue(member.value.get("Status").isEqualString("FAILED")));
        }
      }
    }

    for (auto& memberTry : config) {
      // this should always be true since this future is always fulfilled (even
      // when an exception is thrown)
      TRI_ASSERT(memberTry.hasValue());

      auto& member = memberTry.get();

      {
        VPackObjectBuilder obMember(&builder, member.name);

        builder.add("Role", VPackValue("Agent"));
        builder.add("Endpoint", VPackValue(member.endpoint));
        builder.add("CanBeDeleted", VPackValue(false));

        // check for additional information
        auto info = agents.find(member.name);
        if (info != agents.end()) {
          builder.add("Leading", VPackValue(info->second.leader));
          builder.add("LastAckedTime", VPackValue(info->second.lastAcked));
        }

        if (member.response.hasValue()) {
          if (auto& response = member.response.get();
              response.ok() && response.statusCode() == fuerte::StatusOK) {
            VPackSlice localConfig = response.slice();
            builder.add("Engine", localConfig.get("engine"));
            builder.add("Version", localConfig.get("version"));
            builder.add("Leader", localConfig.get("leaderId"));
            builder.add("Status", VPackValue("GOOD"));
          } else {
            builder.add("Status", VPackValue("BAD"));
          }
        } else {
          builder.add("Status", VPackValue("BAD"));
        }
      }
    }
  }
}

}  // namespace

RestAdminClusterHandler::RestAdminClusterHandler(ArangodServer& server,
                                                 GeneralRequest* request,
                                                 GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

std::string const RestAdminClusterHandler::Health = "health";
std::string const RestAdminClusterHandler::NumberOfServers = "numberOfServers";
std::string const RestAdminClusterHandler::Maintenance = "maintenance";
std::string const RestAdminClusterHandler::NodeVersion = "nodeVersion";
std::string const RestAdminClusterHandler::NodeEngine = "nodeEngine";
std::string const RestAdminClusterHandler::NodeStatistics = "nodeStatistics";
std::string const RestAdminClusterHandler::Statistics = "statistics";
std::string const RestAdminClusterHandler::ShardDistribution =
    "shardDistribution";
std::string const RestAdminClusterHandler::CollectionShardDistribution =
    "collectionShardDistribution";
std::string const RestAdminClusterHandler::CleanoutServer = "cleanOutServer";
std::string const RestAdminClusterHandler::ResignLeadership =
    "resignLeadership";
std::string const RestAdminClusterHandler::MoveShard = "moveShard";
std::string const RestAdminClusterHandler::QueryJobStatus = "queryAgencyJob";
std::string const RestAdminClusterHandler::CancelJob = "cancelAgencyJob";
std::string const RestAdminClusterHandler::RemoveServer = "removeServer";
std::string const RestAdminClusterHandler::RebalanceShards = "rebalanceShards";
std::string const RestAdminClusterHandler::Rebalance = "rebalance";
std::string const RestAdminClusterHandler::ShardStatistics = "shardStatistics";
std::string const RestAdminClusterHandler::VPackSortMigration =
    "vpackSortMigration";
std::string const RestAdminClusterHandler::VPackSortMigrationCheck = "check";
std::string const RestAdminClusterHandler::VPackSortMigrationMigrate =
    "migrate";
std::string const RestAdminClusterHandler::VPackSortMigrationStatus = "status";

RestStatus RestAdminClusterHandler::execute() {
  // here we first do a glboal check, which is based on the setting in startup
  // option
  // `--cluster.api-jwt-policy`:
  // - "jwt-all"    = JWT required to access all operations
  // - "jwt-write"  = JWT required to access post/put/delete operations
  // - "jwt-compat" = compatibility mode = same permissions as in 3.7 (default)
  // this is a convenient way to lock the entire /_admin/cluster API for users
  // w/o JWT.
  if (!ExecContext::current().isSuperuser()) {
    // no superuser... now check if the API policy is set to jwt-all or
    // jwt-write. in this case only requests with valid JWT will have access to
    // the operations (jwt-all = all operations require the JWT, jwt-write =
    // POST/PUT/DELETE operations require the JWT, GET operations are handled as
    // before).
    bool const isWriteOperation =
        (request()->requestType() != rest::RequestType::GET);
    std::string const& apiJwtPolicy =
        server().getFeature<ClusterFeature>().apiJwtPolicy();

    if (apiJwtPolicy == "jwt-all" ||
        (apiJwtPolicy == "jwt-write" && isWriteOperation)) {
      generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
      return RestStatus::DONE;
    }
  }

  // No further check for admin rights here, since we handle this in every
  // individual method below. Some of them do no longer require admin access
  // (e.g. /_admin/cluster/health). If you add a new API below here, please
  // make sure to check for permissions!

  auto const& suffixes = request()->suffixes();
  size_t const len = suffixes.size();

  if (len == 1) {
    std::string const& command = suffixes.at(0);

    if (command == Health) {
      return handleHealth();
    } else if (command == NumberOfServers) {
      return handleNumberOfServers();
    } else if (command == Maintenance) {
      return handleMaintenance();
    } else if (command == NodeVersion) {
      return handleNodeVersion();
    } else if (command == NodeEngine) {
      return handleNodeEngine();
    } else if (command == NodeStatistics) {
      return handleNodeStatistics();
    } else if (command == Statistics) {
      return handleStatistics();
    } else if (command == ShardDistribution) {
      return handleShardDistribution();
    } else if (command == CollectionShardDistribution) {
      return handleCollectionShardDistribution();
    } else if (command == CleanoutServer) {
      return handleCleanoutServer();
    } else if (command == ResignLeadership) {
      return handleResignLeadership();
    } else if (command == MoveShard) {
      return handleMoveShard();
    } else if (command == CancelJob) {
      return handleCancelJob();
    } else if (command == QueryJobStatus) {
      return handleQueryJobStatus();
    } else if (command == RemoveServer) {
      return handleRemoveServer();
    } else if (command == RebalanceShards) {
      return handleRebalanceShards();
    } else if (command == Rebalance) {
      return handleRebalance();
    } else if (command == ShardStatistics) {
      return handleShardStatistics();
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    std::string("invalid command '") + command + "'");
      return RestStatus::DONE;
    }
  } else if (len == 2) {
    std::string const& command = suffixes.at(0);
    if (command == Rebalance) {
      return handleRebalance();
    } else if (command == Maintenance) {
      return handleDBServerMaintenance(suffixes.at(1));
    } else if (command == VPackSortMigration) {
      return waitForFuture(handleVPackSortMigration(suffixes.at(1)));
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    std::string("invalid command '") + command +
                        "', expecting 'rebalance' or 'maintenance'");
      return RestStatus::DONE;
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_SUPERFLUOUS_SUFFICES,
                "expecting URL /_admin/cluster/<command>");
  return RestStatus::DONE;
}

RestAdminClusterHandler::FutureVoid
RestAdminClusterHandler::retryTryDeleteServer(
    std::unique_ptr<RemoveServerContext>&& ctx) {
  if (++ctx->tries < 60) {
    return SchedulerFeature::SCHEDULER->delay("remove-server", 1s)
        .thenValue([this, ctx = std::move(ctx)](auto) mutable {
          return tryDeleteServer(std::move(ctx));
        });
  } else {
    generateError(rest::ResponseCode::PRECONDITION_FAILED,
                  TRI_ERROR_HTTP_PRECONDITION_FAILED,
                  "server may not be deleted");
    return futures::makeFuture();
  }
}

RestAdminClusterHandler::FutureVoid RestAdminClusterHandler::tryDeleteServer(
    std::unique_ptr<RemoveServerContext>&& ctx) {
  auto rootPath = arangodb::cluster::paths::root()->arango();
  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    arangodb::agency::envelope::into_builder(builder)
        .read()
        .key(rootPath->supervision()->health()->str())
        .key(rootPath->plan()->str())
        .key(rootPath->current()->str())
        .end()
        .done();
  }

  return AsyncAgencyComm()
      .sendReadTransaction(20s, std::move(trx))
      .thenValue([this, ctx = std::move(ctx)](
                     AsyncAgencyCommResult&& result) mutable {
        auto rootPath = arangodb::cluster::paths::root()->arango();
        if (result.ok() && result.statusCode() == 200) {
          VPackSlice agency = result.slice().at(0);

          VPackSlice health = agency.get(rootPath->supervision()
                                             ->health()
                                             ->server(ctx->server)
                                             ->status()
                                             ->vec());
          if (!health.isNone()) {
            bool isResponsible = isServerResponsibleForSomething(
                ctx->server, agency.get(rootPath->plan()->vec()),
                agency.get(rootPath->current()->vec()));

            // if the server is still in the list, it was neither in plan nor
            // in current
            if (isResponsible) {
              auto planVersionPath = rootPath->plan()->version();
              // do a write transaction if server is no longer used
              VPackBuffer<uint8_t> trx;
              {
                VPackBuilder builder(trx);
                arangodb::agency::envelope::into_builder(builder)

                    .write()
                    .remove(rootPath->plan()
                                ->coordinators()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->plan()
                                ->dBServers()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->current()
                                ->serversRegistered()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->current()
                                ->dBServers()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->current()
                                ->coordinators()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->supervision()
                                ->health()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->target()
                                ->mapUniqueToShortID()
                                ->server(ctx->server)
                                ->str())
                    .remove(rootPath->current()
                                ->serversKnown()
                                ->server(ctx->server)
                                ->str())
                    .set(rootPath->target()
                             ->removedServers()
                             ->server(ctx->server)
                             ->str(),
                         timepointToString(std::chrono::system_clock::now()))
                    .precs()
                    .isEqual(rootPath->supervision()
                                 ->health()
                                 ->server(ctx->server)
                                 ->status()
                                 ->str(),
                             "FAILED")
                    .isEmpty(rootPath->supervision()
                                 ->dbServers()
                                 ->server(ctx->server)
                                 ->str())
                    .isEqual(planVersionPath->str(),
                             agency.get(planVersionPath->vec()))
                    .end()
                    .done();
              }

              return AsyncAgencyComm()
                  .sendWriteTransaction(20s, std::move(trx))
                  .thenValue([this, ctx = std::move(ctx)](
                                 AsyncAgencyCommResult&& result) mutable {
                    if (result.ok()) {
                      if (result.statusCode() == fuerte::StatusOK) {
                        resetResponse(rest::ResponseCode::OK);
                        return futures::makeFuture();
                      } else if (result.statusCode() ==
                                 fuerte::StatusPreconditionFailed) {
                        TRI_IF_FAILURE("removeServer::noRetry") {
                          generateError(result.asResult());
                          return futures::makeFuture();
                        }
                        return retryTryDeleteServer(std::move(ctx));
                      }
                    }
                    generateError(result.asResult());
                    return futures::makeFuture();
                  });
            }

            TRI_IF_FAILURE("removeServer::noRetry") {
              generateError(Result(TRI_ERROR_HTTP_PRECONDITION_FAILED));
              return futures::makeFuture();
            }

            return retryTryDeleteServer(std::move(ctx));
          } else {
            generateError(rest::ResponseCode::NOT_FOUND,
                          TRI_ERROR_HTTP_NOT_FOUND);
          }
        } else {
          generateError(result.asResult());
        }

        return futures::makeFuture();
      });
}

RestStatus RestAdminClusterHandler::handlePostRemoveServer(
    std::string const& server) {
  auto ctx = std::make_unique<RemoveServerContext>(server);

  return waitForFuture(
      tryDeleteServer(std::move(ctx))
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleRemoveServer() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  VPackSlice server = VPackSlice::noneSlice();
  if (body.isString()) {
    server = body;
  } else if (body.isObject()) {
    server = body.get("server");
  }

  if (server.isString()) {
    std::string serverId = resolveServerNameID(server.copyString());
    return handlePostRemoveServer(serverId);
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                "expecting string or object with key `server`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleShardStatistics() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::NOT_IMPLEMENTED,
                  TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR);
    return RestStatus::DONE;
  }

  if (!_vocbase.isSystem()) {
    generateError(
        GeneralResponse::responseCode(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE),
        TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE);
    return RestStatus::DONE;
  }

  std::string const& restrictServer = _request->value("DBserver");
  bool details = _request->parsedValue("details", false);

  ClusterInfo& ci = server().getFeature<ClusterFeature>().clusterInfo();
  VPackBuilder builder;
  Result res;

  if (restrictServer == "all") {
    res = ci.getShardStatisticsGlobalByServer(builder);
  } else if (details) {
    res = ci.getShardStatisticsGlobalDetailed(restrictServer, builder);
  } else {
    res = ci.getShardStatisticsGlobal(restrictServer, builder);
  }
  if (res.ok()) {
    generateOk(rest::ResponseCode::OK, builder.slice());
  } else {
    generateError(res);
  }

  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCleanoutServer() {
  return handleSingleServerJob("cleanOutServer");
}

RestStatus RestAdminClusterHandler::handleResignLeadership() {
  return handleSingleServerJob("resignLeadership");
}

std::unique_ptr<RestAdminClusterHandler::MoveShardContext>
RestAdminClusterHandler::MoveShardContext::fromVelocyPack(VPackSlice slice) {
  if (slice.isObject()) {
    auto database = slice.get("database");
    auto collection = slice.get("collection");
    auto shard = slice.get("shard");
    auto fromServer = slice.get("fromServer");
    auto toServer = slice.get("toServer");
    auto remainsFollower = slice.get("remainsFollower");
    auto tryUndo = slice.get("tryUndo");

    bool valid = collection.isString() && shard.isString() &&
                 fromServer.isString() && toServer.isString();

    std::string databaseStr =
        database.isString() ? database.copyString() : std::string{};
    if (valid) {
      return std::make_unique<MoveShardContext>(
          std::move(databaseStr), collection.copyString(), shard.copyString(),
          fromServer.copyString(), toServer.copyString(), std::string{},
          remainsFollower.isNone() || remainsFollower.isTrue(),
          tryUndo.isTrue());
    }
  }

  return nullptr;
}

RestStatus RestAdminClusterHandler::handleMoveShard() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  std::unique_ptr<MoveShardContext> ctx =
      MoveShardContext::fromVelocyPack(body);
  if (ctx) {
    if (ctx->database.empty()) {
      ctx->database = _vocbase.name();
    }

    auto const& exec = ExecContext::current();
    bool canAccess = exec.isAdminUser() ||
                     exec.collectionAuthLevel(ctx->database, ctx->collection) ==
                         auth::Level::RW;
    if (!canAccess) {
      generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                    "insufficient permissions on database to move shard");
      return RestStatus::DONE;
    }

    ctx->fromServer = resolveServerNameID(ctx->fromServer);
    ctx->toServer = resolveServerNameID(ctx->toServer);
    return handlePostMoveShard(std::move(ctx));
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                "object with keys `database`, `collection`, `shard`, "
                "`fromServer` and `toServer` (all strings) expected");
  return RestStatus::DONE;
}

RestAdminClusterHandler::FutureVoid RestAdminClusterHandler::createMoveShard(
    std::unique_ptr<MoveShardContext>&& ctx, VPackSlice plan) {
  auto planPath = arangodb::cluster::paths::root()->arango()->plan();

  VPackSlice dbServers = plan.get(planPath->dBServers()->vec());
  bool serversFound = dbServers.isObject() &&
                      dbServers.hasKey(ctx->fromServer) &&
                      dbServers.hasKey(ctx->toServer);
  if (!serversFound) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "one or both dbservers not found");
    return futures::makeFuture();
  }

  VPackSlice collection = plan.get(planPath->collections()
                                       ->database(ctx->database)
                                       ->collection(ctx->collectionID)
                                       ->vec());
  if (!collection.isObject()) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "database/collection not found");
    return futures::makeFuture();
  }

  if (collection.hasKey(StaticStrings::DistributeShardsLike)) {
    generateError(ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "MoveShard only allowed for collections which have "
                  "distributeShardsLike unset.");
    return futures::makeFuture();
  }

  VPackSlice shard =
      collection.get(std::vector<std::string>{"shards", ctx->shard});
  if (!shard.isArray()) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "shard not found");
    return futures::makeFuture();
  }

  bool fromFound = false;
  bool isLeader = false;
  for (velocypack::ArrayIterator it(shard); it.valid(); it.next()) {
    if (it.value().isEqualString(ctx->fromServer)) {
      isLeader = it.index() == 0;
      fromFound = true;
    }
  }

  if (!fromFound) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "shard is not located on the server");
    return futures::makeFuture();
  }

  std::string jobId = std::to_string(
      server().getFeature<ClusterFeature>().clusterInfo().uniqid());
  auto jobToDoPath =
      arangodb::cluster::paths::root()->arango()->target()->toDo()->job(jobId);

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    arangodb::agency::envelope::into_builder(builder)
        .write()
        .emplace(jobToDoPath->str(),
                 [&](VPackBuilder& builder) {
                   builder.add("type", VPackValue("moveShard"));
                   builder.add("database", VPackValue(ctx->database));
                   builder.add("collection", collection.get("id"));
                   builder.add("jobId", VPackValue(jobId));
                   builder.add("shard", VPackValue(ctx->shard));
                   builder.add("fromServer", VPackValue(ctx->fromServer));
                   builder.add("toServer", VPackValue(ctx->toServer));
                   builder.add("isLeader", VPackValue(isLeader));
                   builder.add("remainsFollower",
                               isLeader ? VPackValue(ctx->remainsFollower)
                                        : VPackValue(false));
                   builder.add("tryUndo", VPackValue(ctx->tryUndo));
                   builder.add("creator",
                               VPackValue(ServerState::instance()->getId()));
                   builder.add("timeCreated",
                               VPackValue(timepointToString(
                                   std::chrono::system_clock::now())));
                 })
        .end()
        .done();
  }

  return AsyncAgencyComm()
      .sendWriteTransaction(20s, std::move(trx))
      .thenValue([this, ctx = std::move(ctx),
                  jobId = std::move(jobId)](AsyncAgencyCommResult&& result) {
        if (result.ok() && result.statusCode() == fuerte::StatusOK) {
          VPackBuilder builder;
          ;
          {
            VPackObjectBuilder ob(&builder);
            builder.add("id", VPackValue(jobId));
          }

          generateOk(rest::ResponseCode::ACCEPTED, builder);
        } else {
          generateError(result.asResult());
        }
      });
}

RestStatus RestAdminClusterHandler::handlePostMoveShard(
    std::unique_ptr<MoveShardContext>&& ctx) {
  std::shared_ptr<LogicalCollection> collection =
      server().getFeature<ClusterFeature>().clusterInfo().getCollectionNT(
          ctx->database, ctx->collection);

  if (collection == nullptr) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "database/collection not found");
    return RestStatus::DONE;
  }

  // base64-encode collection id with RevisionId
  ctx->collectionID = RevisionId{collection->planId().id()}.toString();
  auto planPath = arangodb::cluster::paths::root()->arango()->plan();

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    arangodb::agency::envelope::into_builder(builder)
        .read()
        .key(planPath->dBServers()->str())
        .key(planPath->collections()
                 ->database(ctx->database)
                 ->collection(ctx->collectionID)
                 ->str())
        .end()
        .done();
  }

  // gather information about that shard
  return waitForFuture(
      AsyncAgencyComm()
          .sendReadTransaction(20s, std::move(trx))
          .thenValue([this, ctx = std::move(ctx)](
                         AsyncAgencyCommResult&& result) mutable {
            if (result.ok()) {
              switch (result.statusCode()) {
                case fuerte::StatusOK:
                  return createMoveShard(std::move(ctx), result.slice().at(0));
                case fuerte::StatusNotFound:
                  generateError(rest::ResponseCode::NOT_FOUND,
                                TRI_ERROR_HTTP_NOT_FOUND, "unknown collection");
                  return futures::makeFuture();
                default:
                  break;
              }
            }

            generateError(result.asResult());
            return futures::makeFuture();
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleQueryJobStatus() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  std::string jobId = request()->value("id");
  if (jobId.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                  "missing id parameter");
    return RestStatus::DONE;
  }

  auto targetPath = arangodb::cluster::paths::root()->arango()->target();
  std::vector<std::string> paths = {
      targetPath->pending()->job(jobId)->str(),
      targetPath->failed()->job(jobId)->str(),
      targetPath->finished()->job(jobId)->str(),
      targetPath->toDo()->job(jobId)->str(),
  };

  return waitForFuture(
      AsyncAgencyComm()
          .sendTransaction(20s, AgencyReadTransaction{std::move(paths)})
          .thenValue([this, targetPath, jobId = std::move(jobId)](
                         AsyncAgencyCommResult&& result) {
            if (result.ok() && result.statusCode() == fuerte::StatusOK) {
              auto paths = std::vector{
                  targetPath->pending()->job(jobId)->vec(),
                  targetPath->failed()->job(jobId)->vec(),
                  targetPath->finished()->job(jobId)->vec(),
                  targetPath->toDo()->job(jobId)->vec(),
              };

              for (auto const& path : paths) {
                VPackSlice job = result.slice().at(0).get(path);

                if (job.isObject()) {
                  VPackBuffer<uint8_t> payload;
                  {
                    VPackBuilder builder(payload);
                    VPackObjectBuilder ob(&builder);

                    // append all the job keys
                    builder.add(VPackObjectIterator(job));
                    builder.add("error", VPackValue(false));
                    builder.add("job", VPackValue(jobId));
                    builder.add("status", VPackValue(path[2]));
                  }

                  resetResponse(rest::ResponseCode::OK);
                  response()->setPayload(std::move(payload));
                  return;
                }
              }

              generateError(rest::ResponseCode::NOT_FOUND,
                            TRI_ERROR_HTTP_NOT_FOUND);
            } else {
              generateError(result.asResult());
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleCancelJob() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  std::string jobId;
  if (body.isObject() && body.hasKey("id") && body.get("id").isString()) {
    jobId = body.get("id").copyString();
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                  "object with key `id`");
    return RestStatus::DONE;
  }

  auto targetPath = arangodb::cluster::paths::root()->arango()->target();
  std::vector<std::string> paths = {
      targetPath->pending()->job(jobId)->str(),
      targetPath->failed()->job(jobId)->str(),
      targetPath->finished()->job(jobId)->str(),
      targetPath->toDo()->job(jobId)->str(),
  };

  try {
    auto& agencyCache = server().getFeature<ClusterFeature>().agencyCache();
    auto [acb, idx] = agencyCache.read(paths);
    auto res = acb->slice();

    auto paths = std::vector{
        targetPath->pending()->job(jobId)->vec(),
        targetPath->failed()->job(jobId)->vec(),
        targetPath->finished()->job(jobId)->vec(),
        targetPath->toDo()->job(jobId)->vec(),
    };

    for (auto const& path : paths) {
      VPackSlice job = res.at(0).get(path);

      if (job.isObject()) {
        LOG_TOPIC("eb139", INFO, Logger::SUPERVISION)
            << "Attempting to abort supervision job " << job.toJson();
        auto type = job.get("type").stringView();
        // only moveshard and cleanoutserver may be aborted
        if (type != "moveShard" && type != "cleanOutServer") {
          generateError(
              Result{TRI_ERROR_BAD_PARAMETER,
                     "Only MoveShard and CleanOutServer jobs can be aborted"});
          return RestStatus::DONE;
        } else if (path[2] != "Pending" && path[2] != "ToDo") {
          generateError(Result{TRI_ERROR_BAD_PARAMETER,
                               path[2] + " jobs can no longer be cancelled."});
          return RestStatus::DONE;
        }

        // This tranaction aims at killing a job that is todo or pending.
        // A todo job could be pending in the meantime however a pending
        // job can never be todo again. Response ist evaluated in 412 result
        // below.
        auto sendTransaction = [&] {
          VPackBuffer<uint8_t> trxBody;
          {
            VPackBuilder builder(trxBody);
            {
              VPackArrayBuilder trxs(&builder);
              if (path[2] == "ToDo") {
                VPackArrayBuilder trx(&builder);
                {
                  VPackObjectBuilder op(&builder);
                  builder.add("arango/Target/ToDo/" + jobId + "/abort",
                              VPackValue(true));
                }
                {
                  VPackObjectBuilder pre(&builder);
                  builder.add(VPackValue("arango/Target/ToDo/" + jobId));
                  {
                    VPackObjectBuilder val(&builder);
                    builder.add("oldEmpty", VPackValue(false));
                  }
                }
              }
              VPackArrayBuilder trx(&builder);
              {
                VPackObjectBuilder op(&builder);
                builder.add("arango/Target/Pending/" + jobId + "/abort",
                            VPackValue(true));
              }
              {
                VPackObjectBuilder pre(&builder);
                builder.add(VPackValue("arango/Target/Pending/" + jobId));
                {
                  VPackObjectBuilder val(&builder);
                  builder.add("oldEmpty", VPackValue(false));
                }
              }
            }
          }
          return AsyncAgencyComm().sendWriteTransaction(60s,
                                                        std::move(trxBody));
        };

        return waitForFuture(
            sendTransaction()
                .thenValue([=, this](AsyncAgencyCommResult&& wr) {
                  if (!wr.ok()) {
                    // Only if no longer pending or todo.
                    if (wr.statusCode() == 412) {
                      auto results = wr.slice().get("results");
                      if (results[0].getNumber<uint64_t>() == 0 &&
                          results[1].getNumber<uint64_t>() == 0) {
                        generateError(
                            Result{TRI_ERROR_HTTP_PRECONDITION_FAILED,
                                   "Job is no longer pending or to do"});
                        return;
                      }
                    } else {
                      generateError(wr.asResult());
                      return;
                    }
                  }

                  VPackBuffer<uint8_t> payload;
                  {
                    VPackBuilder builder(payload);
                    VPackObjectBuilder ob(&builder);
                    builder.add("job", VPackValue(jobId));
                    builder.add("status", VPackValue("aborted"));
                    builder.add("error", VPackValue(false));
                  }
                  resetResponse(rest::ResponseCode::OK);
                  response()->setPayload(std::move(payload));
                })
                .thenError<VPackException>([this](VPackException const& e) {
                  generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
                })
                .thenError<std::exception>([this](std::exception const& e) {
                  generateError(rest::ResponseCode::SERVER_ERROR,
                                TRI_ERROR_HTTP_SERVER_ERROR, e.what());
                }));
      }
    }

    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);

  } catch (VPackException const& e) {
    generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
  } catch (std::exception const& e) {
    generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR,
                  e.what());
  }

  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleSingleServerJob(
    std::string const& job) {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject()) {
    VPackSlice server = body.get("server");
    if (server.isString()) {
      std::string serverId = resolveServerNameID(server.copyString());
      return handleCreateSingleServerJob(job, serverId, body);
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                "object with key `server`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCreateSingleServerJob(
    std::string const& job, std::string const& serverId, VPackSlice body) {
  std::string jobId = std::to_string(
      server().getFeature<ClusterFeature>().clusterInfo().uniqid());
  auto jobToDoPath =
      arangodb::cluster::paths::root()->arango()->target()->toDo()->job(jobId);

  VPackBuilder builder;
  {
    VPackObjectBuilder ob(&builder);
    builder.add("type", VPackValue(job));
    builder.add("server", VPackValue(serverId));
    builder.add("jobId", VPackValue(jobId));
    builder.add("creator", VPackValue(ServerState::instance()->getId()));
    if (job == "resignLeadership") {
      if (body.isObject()) {
        if (auto undoMoves = body.get("undoMoves"); undoMoves.isBool()) {
          builder.add("undoMoves", VPackValue(undoMoves.isTrue()));
        }

        if (auto waitForInSync = body.get("waitForInSync");
            waitForInSync.isBool()) {
          builder.add("waitForInSync", VPackValue(waitForInSync.isTrue()));
          if (auto timeout = body.get("waitForInSyncTimeout");
              timeout.isNumber<uint64_t>()) {
            builder.add("waitForInSyncTimeout",
                        VPackValue(timeout.getNumber<uint64_t>()));
          }
        }
      }
    }
    builder.add(
        "timeCreated",
        VPackValue(timepointToString(std::chrono::system_clock::now())));
  }

  return waitForFuture(
      AsyncAgencyComm()
          .setValue(20s, jobToDoPath, builder.slice())
          .thenValue(
              [this, jobId = std::move(jobId)](AsyncAgencyCommResult&& result) {
                if (result.ok() && result.statusCode() == 200) {
                  VPackBuilder builder;
                  {
                    VPackObjectBuilder ob(&builder);
                    builder.add("id", VPackValue(jobId));
                  }

                  generateOk(arangodb::rest::ResponseCode::ACCEPTED, builder);
                } else {
                  generateError(result.asResult());
                }
              })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleProxyGetRequest(
    std::string const& url, std::string const& serverFromParameter) {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  std::string const& serverId = request()->value(serverFromParameter);
  if (serverId.empty()) {
    generateError(
        rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
        std::string("missing parameter `") + serverFromParameter + "`");
    return RestStatus::DONE;
  }

  auto* pool = server().getFeature<NetworkFeature>().pool();

  network::RequestOptions opt;
  opt.timeout = 10s;
  auto frequest = network::sendRequestRetry(pool, "server:" + serverId,
                                            fuerte::RestVerb::Get, url,
                                            VPackBuffer<uint8_t>(), opt);

  return waitForFuture(
      std::move(frequest)
          .thenValue([this](network::Response&& result) {
            if (result.ok()) {
              resetResponse(ResponseCode(
                  result.statusCode()));  // I quit if the values are not the
                                          // HTTP Status Codes
              auto payload = result.response().stealPayload();
              response()->setPayload(std::move(*payload));
            } else {
              switch (result.error) {
                case fuerte::Error::ConnectionCanceled:
                  generateError(rest::ResponseCode::BAD,
                                TRI_ERROR_HTTP_BAD_PARAMETER, "unknown server");
                  break;
                case fuerte::Error::CouldNotConnect:
                case fuerte::Error::RequestTimeout:
                  generateError(rest::ResponseCode::REQUEST_TIMEOUT,
                                TRI_ERROR_HTTP_GATEWAY_TIMEOUT,
                                "server did not answer");
                  break;
                default:
                  generateError(rest::ResponseCode::SERVER_ERROR,
                                TRI_ERROR_HTTP_SERVER_ERROR);
              }
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleNodeVersion() {
  return handleProxyGetRequest("/_api/version", "ServerID");
}

RestStatus RestAdminClusterHandler::handleNodeStatistics() {
  return handleProxyGetRequest("/_admin/statistics", "ServerID");
}

RestStatus RestAdminClusterHandler::handleNodeEngine() {
  return handleProxyGetRequest("/_api/engine", "ServerID");
}

RestStatus RestAdminClusterHandler::handleStatistics() {
  return handleProxyGetRequest("/_admin/statistics", "DBserver");
}

RestStatus RestAdminClusterHandler::handleShardDistribution() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  auto reporter = cluster::ShardDistributionReporter::instance(server());

  VPackBuilder builder;
  {
    VPackObjectBuilder obj(&builder);
    builder.add(VPackValue("results"));
    reporter->getDistributionForDatabase(_vocbase.name(), builder);
  }
  generateOk(rest::ResponseCode::OK, builder);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleGetCollectionShardDistribution(
    std::string const& collection) {
  if (collection.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                  "expected nonempty `collection` parameter");
    return RestStatus::DONE;
  }

  auto reporter = cluster::ShardDistributionReporter::instance(server());

  VPackBuilder builder;
  {
    VPackObjectBuilder obj(&builder);
    builder.add(VPackValue("results"));
    reporter->getCollectionDistributionForDatabase(_vocbase.name(), collection,
                                                   builder);
  }
  generateOk(rest::ResponseCode::OK, builder);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCollectionShardDistribution() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetCollectionShardDistribution(
          request()->value("collection"));
    case rest::RequestType::PUT:
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject()) {
    VPackSlice collection = body.get("collection");
    if (collection.isString()) {
      return handleGetCollectionShardDistribution(collection.copyString());
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                "object with key `collection`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleGetMaintenance() {
  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  auto maintenancePath = arangodb::cluster::paths::root()
                             ->arango()
                             ->supervision()
                             ->state()
                             ->mode();

  return waitForFuture(
      AsyncAgencyComm()
          .getValues(maintenancePath)
          .thenValue([this](AgencyReadResult&& result) {
            if (result.ok() && result.statusCode() == fuerte::StatusOK) {
              generateOk(rest::ResponseCode::OK, result.value());
            } else {
              generateError(result.asResult());
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleGetDBServerMaintenance(
    std::string const& serverId) {
  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  auto maintenancePath = arangodb::cluster::paths::root()
                             ->arango()
                             ->current()
                             ->maintenanceDBServers()
                             ->dbserver(serverId);

  return waitForFuture(
      AsyncAgencyComm()
          .getValues(maintenancePath)
          .thenValue([this](AgencyReadResult&& result) {
            if (result.ok() && result.statusCode() == fuerte::StatusOK) {
              generateOk(rest::ResponseCode::OK, result.value());
            } else {
              generateError(result.asResult());
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestAdminClusterHandler::FutureVoid
RestAdminClusterHandler::waitForSupervisionState(
    bool state, std::string const& reactivationTime,
    clock::time_point startTime) {
  return SchedulerFeature::SCHEDULER->delay("wait-for-supervision", 1s)
      .thenValue([](auto) {
        return AsyncAgencyComm().getValues(arangodb::cluster::paths::root()
                                               ->arango()
                                               ->supervision()
                                               ->state()
                                               ->mode());
      })
      .thenValue([this, state, startTime,
                  reactivationTime](AgencyReadResult&& result) {
        auto waitFor = state ? "Maintenance" : "Normal";
        if (result.ok() && result.statusCode() == fuerte::StatusOK) {
          if (!result.value().isEqualString(waitFor)) {
            if ((clock::now() - startTime) < 120.0s) {
              // wait again
              return waitForSupervisionState(state, reactivationTime,
                                             startTime);
            }

            generateError(rest::ResponseCode::REQUEST_TIMEOUT,
                          TRI_ERROR_HTTP_GATEWAY_TIMEOUT,
                          std::string{"timed out while waiting for supervision "
                                      "maintenance mode change"});
          } else {
            auto msg = state
                           ? std::string(
                                 "Cluster supervision deactivated. It will be "
                                 "reactivated automatically on ") +
                                 reactivationTime +
                                 " unless "
                                 "this call is repeated until then."
                           : std::string("Cluster supervision reactivated.");
            VPackBuilder builder;
            VPackBuffer<uint8_t> body;
            {
              VPackObjectBuilder obj(&builder);
              builder.add("warning", VPackValue(msg));
            }
            generateOk(rest::ResponseCode::OK, builder);
          }
        } else {
          generateError(result.asResult());
        }

        return futures::makeFuture();
      });
}

RestStatus RestAdminClusterHandler::setMaintenance(bool wantToActivate,
                                                   uint64_t timeout) {
  // we don't need a timeout when disabling the maintenance
  TRI_ASSERT(wantToActivate || timeout == 0);

  auto maintenancePath =
      arangodb::cluster::paths::root()->arango()->supervision()->maintenance();

  // (stringified) timepoint at which maintenance will reactivate itself
  std::string reactivationTime;
  if (wantToActivate) {
    reactivationTime = timepointToString(std::chrono::system_clock::now() +
                                         std::chrono::seconds(timeout));
  }

  auto sendTransaction = [&] {
    if (wantToActivate) {
      return AsyncAgencyComm().setValue(60s, maintenancePath,
                                        VPackValue(reactivationTime), timeout);
    } else {
      return AsyncAgencyComm().deleteKey(60s, maintenancePath);
    }
  };

  auto self(shared_from_this());

  return waitForFuture(
      sendTransaction()
          .thenValue([this, wantToActivate,
                      reactivationTime](AsyncAgencyCommResult&& result) {
            if (result.ok() && result.statusCode() == 200) {
              return waitForSupervisionState(wantToActivate, reactivationTime,
                                             std::chrono::steady_clock::now());
            } else {
              generateError(result.asResult());
            }
            return futures::makeFuture();
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestAdminClusterHandler::FutureVoid
RestAdminClusterHandler::waitForDBServerMaintenance(std::string const& serverId,
                                                    bool waitForMaintenance) {
  struct Context {
    explicit Context(std::string serverId, bool waitForMaintenance)
        : serverId(std::move(serverId)),
          waitForMaintenance(waitForMaintenance) {}
    futures::Promise<ResultT<consensus::index_t>> promise;
    std::string serverId;
    bool waitForMaintenance;
  };

  auto ctx = std::make_shared<Context>(serverId, waitForMaintenance);
  auto f = ctx->promise.getFuture();

  using namespace cluster::paths;
  // register an agency callback and wait for the given version to appear in
  // target (or bigger)
  auto path = aliases::current()->maintenanceDBServers()->dbserver(serverId);
  auto cb = std::make_shared<AgencyCallback>(
      _vocbase.server(), path->str(SkipComponents(1)),
      [ctx](velocypack::Slice slice, consensus::index_t index) -> bool {
        if (ctx->waitForMaintenance) {
          if (slice.isObject() &&
              slice.get("Mode").isEqualString("maintenance")) {
            ctx->promise.setValue(index);
            return true;
          }
        } else {
          if (slice.isNone()) {
            ctx->promise.setValue(index);
            return true;
          }
        }

        return false;
      },
      true, true);
  auto& cf = server().getFeature<ClusterFeature>();

  if (auto result = cf.agencyCallbackRegistry()->registerCallback(cb);
      result.fail()) {
    return {};
  }

  return std::move(f).then([&cf, cb](auto&& result) {
    cf.agencyCallbackRegistry()->unregisterCallback(cb);
  });
}

RestStatus RestAdminClusterHandler::setDBServerMaintenance(
    std::string const& serverId, std::string const& mode, uint64_t timeout) {
  auto maintenancePath = arangodb::cluster::paths::root()
                             ->arango()
                             ->target()
                             ->maintenanceDBServers()
                             ->dbserver(serverId);
  auto healthPath = arangodb::cluster::paths::root()
                        ->arango()
                        ->supervision()
                        ->health()
                        ->server(serverId)
                        ->status();

  // (stringified) timepoint at which maintenance will reactivate itself
  std::string reactivationTime;
  if (mode == "maintenance") {
    reactivationTime = timepointToString(std::chrono::system_clock::now() +
                                         std::chrono::seconds(timeout));
  }

  auto sendTransaction = [&] {
    if (mode == "maintenance") {
      VPackBuilder builder;
      {
        VPackObjectBuilder guard(&builder);
        builder.add("Mode", VPackValue(mode));
        builder.add("Until", VPackValue(reactivationTime));
      }

      std::vector<AgencyOperation> operations{AgencyOperation{
          maintenancePath->str(cluster::paths::SkipComponents(1)),
          AgencyValueOperationType::SET, builder.slice()}};
      std::vector<AgencyPrecondition> preconds{AgencyPrecondition{
          healthPath->str(cluster::paths::SkipComponents(1)),
          AgencyPrecondition::Type::VALUE, VPackValue("GOOD")}};
      AgencyWriteTransaction trx(operations, preconds);
      return AsyncAgencyComm().sendTransaction(60s, trx);
    } else {
      std::vector<AgencyOperation> operations{AgencyOperation{
          maintenancePath->str(cluster::paths::SkipComponents(1)),
          AgencySimpleOperationType::DELETE_OP}};
      std::vector<AgencyPrecondition> preconds{AgencyPrecondition{
          healthPath->str(cluster::paths::SkipComponents(1)),
          AgencyPrecondition::Type::VALUE, VPackValue("GOOD")}};
      AgencyWriteTransaction trx(operations, preconds);
      return AsyncAgencyComm().sendTransaction(60s, trx);
    }
  };

  auto self(shared_from_this());
  bool const isMaintenanceMode = mode == "maintenance";

  return waitForFuture(
      sendTransaction()
          .thenValue([this, reactivationTime, isMaintenanceMode,
                      serverId](AsyncAgencyCommResult&& result) {
            if (result.ok() && result.statusCode() == 200) {
              VPackBuilder builder;
              { VPackObjectBuilder obj(&builder); }
              generateOk(rest::ResponseCode::OK, builder);
              return waitForDBServerMaintenance(serverId, isMaintenanceMode);
            } else {
              generateError(result.asResult());
            }
            return futures::makeFuture();
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handlePutMaintenance() {
  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isString()) {
    if (body.isEqualString("on")) {
      // supervision maintenance will turn itself off automatically
      // after one hour by default
      constexpr uint64_t timeout = 3600;  // 1 hour
      return setMaintenance(true, timeout);
    } else if (body.isEqualString("off")) {
      // timeout doesn't matter for turning off the supervision
      return setMaintenance(false, /*timeout*/ 0);
    } else {
      // user sent a stringified timeout value, so lets honor this
      VPackValueLength l;
      char const* p = body.getString(l);
      bool valid = false;
      uint64_t timeout = NumberUtils::atoi_positive<uint64_t>(p, p + l, valid);
      if (valid) {
        return setMaintenance(true, timeout);
      }
      // otherwise fall-through to error
    }
  } else if (body.isNumber()) {
    // user sent a numeric timeout value, so lets honor this
    uint64_t timeout = body.getNumber<uint64_t>();
    return setMaintenance(true, timeout);
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                "string expected with value `on` or `off`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handlePutDBServerMaintenance(
    std::string const& serverId) {
  TRI_ASSERT(AsyncAgencyCommManager::INSTANCE != nullptr);

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject() && body.hasKey("mode")) {
    std::string mode = body.get("mode").copyString();
    if (mode == "maintenance" || mode == "normal") {
      uint64_t timeout = 3600;  // 1 hour
      if (body.hasKey("timeout")) {
        VPackSlice timeoutSlice = body.get("timeout");
        if (timeoutSlice.isInteger()) {
          try {
            timeout = timeoutSlice.getNumber<uint64_t>();
          } catch (std::exception const&) {
            // If the value is negative or a float, we simply keep 3600
          }
        }
      }
      return setDBServerMaintenance(serverId, mode, timeout);
    }
    // otherwise fall-through to error
  }

  generateError(
      rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
      "object expected with attribute 'mode' and optional attribute 'timeout'");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleMaintenance() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator() &&
      !ServerState::instance()->isSingleServer()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on single server and coordinators");
    return RestStatus::DONE;
  }

  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetMaintenance();
    case rest::RequestType::PUT:
      return handlePutMaintenance();
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }
}

RestStatus RestAdminClusterHandler::handleDBServerMaintenance(
    std::string const& serverId) {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  TRI_ASSERT(AsyncAgencyCommManager::INSTANCE != nullptr);

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetDBServerMaintenance(serverId);
    case rest::RequestType::PUT:
      return handlePutDBServerMaintenance(serverId);
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }
}

RestStatus RestAdminClusterHandler::handleGetNumberOfServers() {
  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  auto targetPath = arangodb::cluster::paths::root()->arango()->target();

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    arangodb::agency::envelope::into_builder(builder)
        .read()
        .key(targetPath->numberOfDBServers()->str())
        .key(targetPath->numberOfCoordinators()->str())
        .key(targetPath->cleanedServers()->str())
        .end()
        .done();
  }

  auto self(shared_from_this());
  return waitForFuture(
      AsyncAgencyComm()
          .sendReadTransaction(10.0s, std::move(trx))
          .thenValue([this](AsyncAgencyCommResult&& result) {
            auto targetPath =
                arangodb::cluster::paths::root()->arango()->target();

            if (result.ok() && result.statusCode() == fuerte::StatusOK) {
              VPackBuilder builder;
              {
                VPackObjectBuilder ob(&builder);
                builder.add("numberOfDBServers",
                            result.slice().at(0).get(
                                targetPath->numberOfDBServers()->vec()));
                builder.add("numberOfCoordinators",
                            result.slice().at(0).get(
                                targetPath->numberOfCoordinators()->vec()));
                builder.add("cleanedServers",
                            result.slice().at(0).get(
                                targetPath->cleanedServers()->vec()));
              }

              generateOk(rest::ResponseCode::OK, builder);
            } else {
              generateError(rest::ResponseCode::SERVER_ERROR,
                            TRI_ERROR_HTTP_SERVER_ERROR,
                            "agency communication failed");
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handlePutNumberOfServers() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                  "object expected");
    return RestStatus::DONE;
  }

  std::vector<AgencyOperation> ops;
  auto targetPath = arangodb::cluster::paths::root()->arango()->target();
  bool hasThingsToDo = false;

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    auto write = arangodb::agency::envelope::into_builder(builder).write();

    VPackSlice numberOfCoordinators = body.get("numberOfCoordinators");
    if (numberOfCoordinators.isNumber() || numberOfCoordinators.isNull()) {
      write = std::move(write).set(targetPath->numberOfCoordinators()->str(),
                                   numberOfCoordinators);
      hasThingsToDo = true;
    } else if (!numberOfCoordinators.isNone()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                    "numberOfCoordinators: number expected");
      return RestStatus::DONE;
    }

    VPackSlice numberOfDBServers = body.get("numberOfDBServers");
    if (numberOfDBServers.isNumber() || numberOfDBServers.isNull()) {
      write = std::move(write).set(targetPath->numberOfDBServers()->str(),
                                   numberOfDBServers);
      hasThingsToDo = true;
    } else if (!numberOfDBServers.isNone()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                    "numberOfDBServers: number expected");
      return RestStatus::DONE;
    }

    VPackSlice cleanedServers = body.get("cleanedServers");
    if (cleanedServers.isArray()) {
      bool allStrings = true;
      for (auto server : VPackArrayIterator(cleanedServers)) {
        if (!server.isString()) {
          allStrings = false;
          break;
        }
      }

      if (allStrings) {
        write = std::move(write).set(targetPath->cleanedServers()->str(),
                                     cleanedServers);
        hasThingsToDo = true;
      } else {
        generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                      "cleanedServers: array of strings expected");
        return RestStatus::DONE;
      }
    } else if (!cleanedServers.isNone()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
                    "cleanedServers: array expected");
      return RestStatus::DONE;
    }

    std::move(write).end().done();
  }

  if (!hasThingsToDo) {
    generateOk(rest::ResponseCode::OK, velocypack::Slice::noneSlice());
    // TODO: the appropriate response would rather be
    // generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER,
    //               "missing fields");
    // but that would break API compatibility. Introduce this behavior
    // in 4.0!!
    return RestStatus::DONE;
  }

  auto self(shared_from_this());
  return waitForFuture(
      AsyncAgencyComm()
          .sendWriteTransaction(20s, std::move(trx))
          .thenValue([this](AsyncAgencyCommResult&& result) {
            if (result.ok() && result.statusCode() == fuerte::StatusOK) {
              generateOk(rest::ResponseCode::OK, VPackSlice::noneSlice());
            } else {
              generateError(result.asResult());
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

RestStatus RestAdminClusterHandler::handleNumberOfServers() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  // GET requests are allowed for everyone, unless --server.harden is used.
  // in this case admin privileges are required.
  // PUT requests always require admin privileges
  ServerSecurityFeature& security =
      server().getFeature<ServerSecurityFeature>();
  bool const needsAdminPrivileges =
      (request()->requestType() != rest::RequestType::GET ||
       security.isRestApiHardened());

  if (needsAdminPrivileges && !ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetNumberOfServers();
    case rest::RequestType::PUT:
      return handlePutNumberOfServers();
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }
}

RestStatus RestAdminClusterHandler::handleHealth() {
  // We allow this API whenever one is authenticated in some way. There used
  // to be a check for isAdminUser here. However, we want that the UI with
  // the cluster health dashboard works for every authenticated user.
  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator() &&
      !ServerState::instance()->isSingleServer()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on single server and coordinators");
    return RestStatus::DONE;
  }

  if (AsyncAgencyCommManager::INSTANCE == nullptr) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "not allowed on single servers");
    return RestStatus::DONE;
  }

  // TODO handle timeout parameter

  auto self(shared_from_this());

  // query the agency config
  auto fConfig =
      AsyncAgencyComm()
          .sendWithFailover(fuerte::RestVerb::Get, "/_api/agency/config", 60.0s,
                            AsyncAgencyComm::RequestType::READ,
                            VPackBuffer<uint8_t>())
          .thenValue([self](AsyncAgencyCommResult&& result) {
            // this lambda has to capture self since collect returns early on
            // an exception and the RestHandler might be freed too early
            // otherwise

            if (result.fail() || result.statusCode() != fuerte::StatusOK) {
              THROW_ARANGO_EXCEPTION(result.asResult());
            }

            // now connect to all the members and ask for their engine and
            // version
            std::vector<futures::Future<::agentConfigHealthResult>> fs;

            auto* pool = self->server().getFeature<NetworkFeature>().pool();
            for (auto member : VPackObjectIterator(result.slice().get(
                     std::vector<std::string>{"configuration", "pool"}))) {
              std::string endpoint = member.value.copyString();
              std::string memberName = member.key.copyString();

              auto future =
                  network::sendRequest(pool, endpoint, fuerte::RestVerb::Get,
                                       "/_api/agency/config",
                                       VPackBuffer<uint8_t>())
                      .then(
                          [endpoint = std::move(endpoint),
                           memberName = std::move(memberName)](
                              futures::Try<network::Response>&& resp) mutable {
                            return futures::makeFuture(
                                ::agentConfigHealthResult{std::move(endpoint),
                                                          std::move(memberName),
                                                          std::move(resp)});
                          });

              fs.emplace_back(std::move(future));
            }

            return futures::collectAll(fs);
          });

  // query information from the store
  auto rootPath = arangodb::cluster::paths::root()->arango();
  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    arangodb::agency::envelope::into_builder(builder)
        .read()
        .key(rootPath->cluster()->str())
        .key(rootPath->supervision()->health()->str())
        .key(rootPath->plan()->str())
        .key(rootPath->current()->str())
        .end()
        .done();
  }
  auto fStore = AsyncAgencyComm().sendReadTransaction(60.0s, std::move(trx));

  return waitForFuture(
      futures::collect(std::move(fConfig), std::move(fStore))
          .thenValue([this](auto&& result) {
            auto rootPath = arangodb::cluster::paths::root()->arango();
            auto& [configResult, storeResult] = result;
            if (storeResult.ok() &&
                storeResult.statusCode() == fuerte::StatusOK) {
              VPackBuilder builder;
              {
                VPackObjectBuilder ob(&builder);
                ::buildHealthResult(builder, configResult,
                                    storeResult.slice().at(0));
              }
              generateOk(rest::ResponseCode::OK, builder);
            } else {
              generateError(rest::ResponseCode::SERVER_ERROR,
                            TRI_ERROR_HTTP_SERVER_ERROR,
                            "agency communication failed");
            }
          })
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

std::string RestAdminClusterHandler::resolveServerNameID(
    std::string const& serverName) {
  auto servers =
      server().getFeature<ClusterFeature>().clusterInfo().getServerAliases();

  for (auto const& pair : servers) {
    if (pair.second == serverName) {
      return pair.first;
    }
  }

  return serverName;
}

namespace std {
template<>
struct hash<RestAdminClusterHandler::CollectionShardPair> {
  std::size_t operator()(
      RestAdminClusterHandler::CollectionShardPair const& a) const {
    std::size_t seed = 0;
    boost::hash_combine(seed, std::hash<std::string>()(a.collection));
    boost::hash_combine(seed, std::hash<std::string>()(a.shard));
    return seed;
  }
};
}  // namespace std

void RestAdminClusterHandler::getShardDistribution(
    std::map<std::string, std::unordered_set<CollectionShardPair>>& distr) {
  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  for (auto const& server : ci.getCurrentDBServers()) {
    distr[server].clear();
  }

  for (auto const& collection : ci.getCollections(_vocbase.name())) {
    if (!collection->distributeShardsLike().empty()) {
      continue;
    }
    // base64-encode collection id with RevisionId
    std::string collectionID = RevisionId{collection->planId().id()}.toString();
    auto shardIds = collection->shardIds();
    for (auto const& shard : *shardIds) {
      for (size_t i = 0; i < shard.second.size(); i++) {
        std::string const& server = shard.second.at(i);
        distr[server].emplace(
            CollectionShardPair{collectionID, shard.first, i == 0});
      }
    }
  }
}

namespace {

using CollectionShardPair = RestAdminClusterHandler::CollectionShardPair;
using MoveShardDescription = RestAdminClusterHandler::MoveShardDescription;

void theSimpleStupidOne(
    std::map<std::string, std::unordered_set<CollectionShardPair>>& shardMap,
    std::vector<MoveShardDescription>& moves, std::uint32_t numMoveShards,
    DatabaseID database) {
  // If you dislike this algorithm feel free to add a new one.
  // shardMap is a map from dbserver to a set of shards located on that
  // server. your algorithm has to fill `moves` with the move shard operations
  // that it wants to execute. Please fill in all values of the
  // `MoveShardDescription` struct.

  std::unordered_set<std::string> movedShards;
  while (moves.size() < numMoveShards) {
    auto [emptiest, fullest] = std::minmax_element(
        shardMap.begin(), shardMap.end(), [](auto const& a, auto const& b) {
          return a.second.size() < b.second.size();
        });

    if (emptiest->second.size() + 1 >= fullest->second.size()) {
      break;
    }

    auto pair = fullest->second.begin();
    for (; pair != fullest->second.end(); pair++) {
      if (movedShards.count(pair->shard) != 0) {
        continue;
      }

      auto i = std::find_if(
          emptiest->second.begin(), emptiest->second.end(),
          [&](CollectionShardPair const& p) { return p.shard == pair->shard; });
      if (i == emptiest->second.end()) {
        break;
      }
    }

    if (pair == fullest->second.end()) {
      break;
    }

    // move the shard
    moves.push_back(MoveShardDescription{database, pair->collection,
                                         pair->shard, fullest->first,
                                         emptiest->first, pair->isLeader});
    movedShards.insert(pair->shard);
    emptiest->second.emplace(*pair);
    fullest->second.erase(pair);
  }
}
}  // namespace

RestAdminClusterHandler::FutureVoid
RestAdminClusterHandler::handlePostRebalanceShards(
    const ReshardAlgorithm& algorithm) {
  // dbserver -> shards
  std::vector<MoveShardDescription> moves;
  std::map<std::string, std::unordered_set<CollectionShardPair>> shardMap;
  getShardDistribution(shardMap);

  algorithm(shardMap, moves,
            server().getFeature<ClusterFeature>().maxNumberOfMoveShards(),
            _vocbase.name());

  VPackBuilder responseBuilder;
  responseBuilder.openObject();
  responseBuilder.add("operations", VPackValue(moves.size()));
  responseBuilder.close();

  if (moves.empty()) {
    generateOk(rest::ResponseCode::OK, responseBuilder.slice());
    return futures::makeFuture();
  }

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    auto write = arangodb::agency::envelope::into_builder(builder).write();

    auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
    std::string timestamp = timepointToString(std::chrono::system_clock::now());
    for (auto const& move : moves) {
      std::string jobId = std::to_string(ci.uniqid());
      auto jobToDoPath =
          arangodb::cluster::paths::root()->arango()->target()->toDo()->job(
              jobId);
      write = std::move(write).emplace(
          jobToDoPath->str(), [&](VPackBuilder& builder) {
            builder.add("type", VPackValue("moveShard"));
            builder.add("database", VPackValue(_vocbase.name()));
            builder.add("collection", VPackValue(move.collection));
            builder.add("jobId", VPackValue(jobId));
            builder.add("shard", VPackValue(move.shard));
            builder.add("fromServer", VPackValue(move.from));
            builder.add("toServer", VPackValue(move.to));
            builder.add("isLeader", VPackValue(move.isLeader));
            builder.add("remainsFollower", VPackValue(move.isLeader));
            builder.add("creator",
                        VPackValue(ServerState::instance()->getId()));
            builder.add("timeCreated", VPackValue(timestamp));
          });
    }
    std::move(write).end().done();
  }

  return AsyncAgencyComm()
      .sendWriteTransaction(20s, std::move(trx))
      .thenValue([this, resBuilder = std::move(responseBuilder)](
                     AsyncAgencyCommResult&& result) {
        if (result.ok() && result.statusCode() == 200) {
          generateOk(rest::ResponseCode::ACCEPTED, resBuilder.slice());
        } else {
          generateError(result.asResult());
        }
      });
}

RestStatus RestAdminClusterHandler::handleRebalanceShards() {
  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  ExecContext const& exec = ExecContext::current();
  if (!exec.canUseDatabase(auth::Level::RW)) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "insufficient permissions");
    return RestStatus::DONE;
  }

  // ADD YOUR ALGORITHM HERE!!!

  std::string algorithmName = request()->value("algorithm");
  ReshardAlgorithm algorithm;
  if (algorithmName == "simple" || algorithmName.empty()) {
    algorithm = theSimpleStupidOne;
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "unknown algorithm");
    return RestStatus::DONE;
  }

  return waitForFuture(
      handlePostRebalanceShards(algorithm)
          .thenError<VPackException>([this](VPackException const& e) {
            generateError(Result{TRI_ERROR_HTTP_SERVER_ERROR, e.what()});
          })
          .thenError<std::exception>([this](std::exception const& e) {
            generateError(rest::ResponseCode::SERVER_ERROR,
                          TRI_ERROR_HTTP_SERVER_ERROR, e.what());
          }));
}

namespace {
struct RebalanceOptions {
  std::uint64_t version;
  std::size_t maximumNumberOfMoves;
  bool leaderChanges;
  bool moveLeaders;
  bool moveFollowers;
  bool excludeSystemCollections;
  double piFactor;
  std::vector<DatabaseID> databasesExcluded;
};

template<class Inspector>
auto inspect(Inspector& f, RebalanceOptions& x) {
  return f.object(x).fields(
      f.field("version", x.version),
      f.field("maximumNumberOfMoves", x.maximumNumberOfMoves)
          .fallback(std::size_t{1000}),
      f.field("leaderChanges", x.leaderChanges).fallback(true),
      f.field("moveLeaders", x.moveLeaders).fallback(false),
      f.field("moveFollowers", x.moveFollowers).fallback(false),
      f.field("excludeSystemCollections", x.excludeSystemCollections)
          .fallback(false),
      f.field("piFactor", x.piFactor).fallback(1.0),
      f.field("databasesExcluded", x.databasesExcluded)
          .fallback(std::vector<DatabaseID>{}));
};

}  // namespace

RestAdminClusterHandler::MoveShardCount
RestAdminClusterHandler::countAllMoveShardJobs() {
  auto& cache = server().getFeature<ClusterFeature>().agencyCache();

  auto const countMoveShardsInSlice = [](VPackSlice slice) -> std::size_t {
    std::size_t count = 0;
    for (auto const& [key, job] : VPackObjectIterator(slice)) {
      if (job.get("type").isEqualString("moveShard")) {
        count += 1;
      }
    }

    return count;
  };

  auto const countMoveShardsAt = [&](std::string const& path) -> std::size_t {
    auto [query, index] = cache.get(path);
    return countMoveShardsInSlice(query->slice());
  };
  return MoveShardCount{
      .todo = countMoveShardsAt("Target/ToDo"),
      .pending = countMoveShardsAt("Target/Pending"),
  };
}

RestStatus RestAdminClusterHandler::handleRebalanceGet() {
  if (request()->suffixes().size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_admin/cluster/rebalance");
    return RestStatus::DONE;
  }

  auto [todo, pending] = countAllMoveShardJobs();

  VPackBuilder builder;
  auto p = collectRebalanceInformation({}, false);
  auto leader = p.computeLeaderImbalance();
  auto shard = p.computeShardImbalance();
  {
    VPackObjectBuilder ob(&builder);
    builder.add(VPackValue("leader"));
    velocypack::serialize(builder, leader);
    builder.add(VPackValue("shards"));
    velocypack::serialize(builder, shard);
    builder.add("pendingMoveShards", VPackValue(pending));
    builder.add("todoMoveShards", VPackValue(todo));
  }

  generateOk(rest::ResponseCode::OK, builder.slice());
  return RestStatus::DONE;
}

namespace {
struct RebalanceExecuteOptions {
  std::vector<MoveShardDescription> moves;
  std::uint64_t version{0};
};

template<class Inspector>
auto inspect(Inspector& f, RebalanceExecuteOptions& x) {
  return f.object(x).fields(f.field("moves", x.moves),
                            f.field("version", x.version));
}

template<typename T, typename F>
futures::Future<Result> executeMoveShardOperations(std::vector<T> const& batch,
                                                   ClusterInfo& ci,
                                                   F const& mapper) {
  if (batch.empty()) {
    return Result{};
  }

  VPackBuffer<uint8_t> trx;
  {
    VPackBuilder builder(trx);
    auto write = arangodb::agency::envelope::into_builder(builder).write();

    std::string timestamp = timepointToString(std::chrono::system_clock::now());
    for (auto const& move : batch) {
      auto const& mappedMove = mapper(move);
      std::string jobId = std::to_string(ci.uniqid());
      auto jobToDoPath =
          arangodb::cluster::paths::root()->arango()->target()->toDo()->job(
              jobId);
      write = std::move(write).emplace(
          jobToDoPath->str(), [&](VPackBuilder& builder) {
            builder.add("type", VPackValue("moveShard"));
            builder.add("database", VPackValue(mappedMove.database));
            builder.add("collection", VPackValue(mappedMove.collection));
            builder.add("jobId", VPackValue(jobId));
            builder.add("shard", VPackValue(mappedMove.shard));
            builder.add("fromServer", VPackValue(mappedMove.from));
            builder.add("toServer", VPackValue(mappedMove.to));
            builder.add("isLeader", VPackValue(mappedMove.isLeader));
            builder.add("creator",
                        VPackValue(ServerState::instance()->getId()));
            builder.add("timeCreated", VPackValue(timestamp));
          });
    }
    std::move(write).end().done();
  }

  return AsyncAgencyComm()
      .sendWriteTransaction(20s, std::move(trx))
      .thenValue([](AsyncAgencyCommResult&& result) {
        if (result.ok() && result.statusCode() == 200) {
          return Result{};
        } else {
          return result.asResult();
        }
      });
}
}  // namespace

RestStatus RestAdminClusterHandler::handleRebalanceExecute() {
  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }
  auto opts =
      velocypack::deserialize<RebalanceExecuteOptions>(_request->payload());

  if (opts.version != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "unknown version provided");
    return RestStatus::DONE;
  }

  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  auto const idMapper = [](MoveShardDescription const& desc) -> auto& {
    return desc;
  };

  if (opts.moves.empty()) {
    generateOk(rest::ResponseCode::OK, VPackSlice::noneSlice());
    return RestStatus::DONE;
  }

  return waitForFuture(executeMoveShardOperations(opts.moves, ci, idMapper)
                           .thenValue([this](auto&& result) {
                             if (result.ok()) {
                               generateOk(rest::ResponseCode::ACCEPTED,
                                          VPackSlice::noneSlice());
                             } else {
                               generateError(result);
                             }
                           }));
}

RestStatus RestAdminClusterHandler::handleRebalancePlan() {
  using namespace cluster::rebalance;

  auto const readRebalanceOptions = [&]() -> std::optional<RebalanceOptions> {
    auto opts = velocypack::deserialize<RebalanceOptions>(_request->payload());
    if (opts.maximumNumberOfMoves > 5000) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "at most 5000 moves allowed");
      return std::nullopt;
    }

    if (opts.version != 1) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "unknown version provided");
      return std::nullopt;
    }

    return opts;
  };

  std::vector<MoveShardJob> moves;

  auto options = readRebalanceOptions();
  if (!options) {
    return RestStatus::DONE;
  }

  auto p = collectRebalanceInformation(options->databasesExcluded,
                                       options->excludeSystemCollections);
  p.setPiFactor(options->piFactor);
  auto const imbalanceLeaderBefore = p.computeLeaderImbalance();
  auto const imbalanceShardsBefore = p.computeShardImbalance();

  moves.reserve(options->maximumNumberOfMoves);
  p.optimize(options->leaderChanges, options->moveFollowers,
             options->moveLeaders, options->maximumNumberOfMoves, moves);

  for (auto const& move : moves) {
    p.applyMoveShardJob(move, false, nullptr, nullptr);
  }

  auto const imbalanceLeaderAfter = p.computeLeaderImbalance();
  auto const imbalanceShardsAfter = p.computeShardImbalance();

  auto const buildResponse = [&](rest::ResponseCode responseCode) {
    {
      VPackBuilder builder;
      {
        VPackObjectBuilder ob1(&builder);
        {
          VPackObjectBuilder ob(&builder, "imbalanceBefore");
          builder.add(VPackValue("leader"));
          velocypack::serialize(builder, imbalanceLeaderBefore);
          builder.add(VPackValue("shards"));
          velocypack::serialize(builder, imbalanceShardsBefore);
        }
        {
          VPackObjectBuilder ob(&builder, "imbalanceAfter");
          builder.add(VPackValue("leader"));
          velocypack::serialize(builder, imbalanceLeaderAfter);
          builder.add(VPackValue("shards"));
          velocypack::serialize(builder, imbalanceShardsAfter);
        }
        {
          VPackArrayBuilder ab(&builder, "moves");
          for (auto const& move : moves) {
            auto const& shard = p.shards[move.shardId];
            auto const& collection = p.collections[shard.collectionId];
            VPackObjectBuilder ob(&builder);
            builder.add("from", VPackValue(p.dbServers[move.from].id));
            builder.add("to", VPackValue(p.dbServers[move.to].id));
            builder.add("shard", VPackValue(shard.name));
            builder.add("collection", VPackValue(collection.name));
            builder.add("database",
                        VPackValue(p.databases[collection.dbId].name));
            builder.add("isLeader", VPackValue(move.isLeader));
          }
        }
      }
      generateOk(responseCode, builder.slice());
    }
  };
  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  auto const moveShardConverter = [&](MoveShardJob const& job) {
    auto& shard = p.shards[job.shardId];
    auto& col = p.collections[shard.collectionId];

    return MoveShardDescription{p.databases[col.dbId].name,
                                col.name,
                                shard.name,
                                p.dbServers[job.from].id,
                                p.dbServers[job.to].id,
                                job.isLeader};
  };

  switch (request()->requestType()) {
    case rest::RequestType::POST: {
      buildResponse(rest::ResponseCode::OK);
      return RestStatus::DONE;
    }
    case rest::RequestType::PUT: {
      buildResponse(rest::ResponseCode::ACCEPTED);
      return waitForFuture(
          executeMoveShardOperations(moves, ci, moveShardConverter)
              .thenValue([&](auto&& result) {
                if (!result.ok()) {
                  generateError(result);
                }
              }));
    }
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }
}

RestStatus RestAdminClusterHandler::handleRebalance() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN,
                  "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() == rest::RequestType::GET) {
    return handleRebalanceGet();
  }

  bool parseSuccess = false;
  std::ignore = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {  // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  if (auto const& suff = request()->suffixes(); suff.size() == 2) {
    if (suff[1] != "execute") {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expect /_admin/cluster/rebalance[/execute]");
      return RestStatus::DONE;
    }
    return handleRebalanceExecute();
  }

  return handleRebalancePlan();
}

cluster::rebalance::AutoRebalanceProblem
RestAdminClusterHandler::collectRebalanceInformation(
    std::vector<std::string> const& excludedDatabases,
    bool excludeSystemCollections) {
  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  cluster::rebalance::AutoRebalanceProblem p;
  p.zones.emplace_back(cluster::rebalance::Zone{.id = "ZONE"});

  std::string const healthPath = "Supervision/Health";

  auto& cache = server().getFeature<ClusterFeature>().agencyCache();
  auto [acb, idx] =
      cache.read(std::vector<std::string>{AgencyCommHelper::path(healthPath)});
  auto agencyCacheInfo = acb->slice();

  if (!agencyCacheInfo.isArray()) {
    generateError(ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR,
                  "Failed to acquire endpoints from agency cache");
  }

  auto serversHealthInfo =
      agencyCacheInfo[0].get({"arango", "Supervision", "Health"});

  std::unordered_set<std::string> activeServers;
  for (auto it : velocypack::ObjectIterator(serversHealthInfo)) {
    if (it.value.get("Status").stringView() ==
        consensus::Supervision::HEALTH_STATUS_GOOD) {
      activeServers.emplace(it.key.copyString());
    }
  }

  p.setServersHealthInfo(std::move(activeServers));

  std::unordered_map<ServerID, std::uint32_t> serverToIndex;

  auto const getDBServerIndex =
      [&](std::string const& server) -> std::uint32_t {
    auto serverName = server;
    if (serverName.starts_with("_")) {
      serverName = serverName.substr(1);
    }

    if (auto iter = serverToIndex.find(serverName);
        iter != std::end(serverToIndex)) {
      return iter->second;
    } else {
      auto idx = serverToIndex[serverName] =
          static_cast<std::uint32_t>(p.dbServers.size());
      auto& ref = p.dbServers.emplace_back();
      ref.id = serverName;
      ref.zone = 0;
      ref.volumeSize = 7000000000;
      ref.freeDiskSize = 5000000000;
      ref.CPUcapacity = 32;
      return idx;
    }
  };

  for (auto const& server : ci.getCurrentDBServers()) {
    std::ignore = getDBServerIndex(server);
  }

  for (auto const& db : ci.databases()) {
    if (std::find(excludedDatabases.begin(), excludedDatabases.end(), db) !=
        excludedDatabases.end()) {
      continue;
    }

    std::size_t dbIndex = p.databases.size();
    auto& databaseRef = p.databases.emplace_back();
    databaseRef.id = dbIndex;
    databaseRef.name = db;

    struct CollectionMetaData {
      std::size_t index = 0;
      std::size_t distributeShardsLikeCounter = 0;
    };

    std::unordered_map<std::string, CollectionMetaData>
        distributeShardsLikeCounter;

    for (auto const& collection : ci.getCollections(db)) {
      bool isSystem = collection->name().starts_with("_");
      if (excludeSystemCollections && isSystem) {
        continue;
      }
      if (auto const& like = collection->distributeShardsLike();
          !like.empty()) {
        distributeShardsLikeCounter[like].distributeShardsLikeCounter += 1;
      } else {
        std::size_t index = p.collections.size();
        auto& collectionRef = p.collections.emplace_back();
        collectionRef.id = index;
        collectionRef.name = std::to_string(collection->id().id());
        collectionRef.dbId = dbIndex;
        collectionRef.weight = 1.0;
        distributeShardsLikeCounter[collectionRef.name].index = index;

        auto shardIds = collection->shardIds();
        for (auto const& shard : *shardIds) {
          auto shardIndex =
              static_cast<decltype(collectionRef.shards)::value_type>(
                  p.shards.size());
          collectionRef.shards.push_back(shardIndex);
          auto& shardRef = p.shards.emplace_back();
          shardRef.name = shard.first;
          TRI_ASSERT(!shard.second.empty());
          shardRef.leader = getDBServerIndex(shard.second[0]);
          shardRef.id = shardIndex;
          shardRef.collectionId = index;
          shardRef.isSystem = isSystem;
          shardRef.replicationFactor =
              static_cast<decltype(shardRef.replicationFactor)>(
                  shard.second.size());
          shardRef.weight = 1.;
          shardRef.size = 1024 * 1024;  // size of data in that shard
          bool first = true;
          for (auto const& server : shard.second) {
            if (first) {
              first = false;
              continue;
            }
            shardRef.followers.emplace_back(getDBServerIndex(server));
          }
        }
      }
    }

    for (auto const& [id, count] : distributeShardsLikeCounter) {
      for (auto const& shardId : p.collections[count.index].shards) {
        p.shards[shardId].weight += count.distributeShardsLikeCounter;
      }
    }
  }

  return p;
}

RestAdminClusterHandler::FutureVoid
RestAdminClusterHandler::handleVPackSortMigration(
    std::string const& subCommand) {
  // First we do the authentication: We only allow superuser access, since
  // this is a critical migration operation:
  if (ExecContext::isAuthEnabled() && !ExecContext::current().isSuperuser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN,
                  "only superusers may run vpack index migration");
    co_return;
  }

  // First check methods:
  if (!((request()->requestType() == rest::RequestType::GET &&
         subCommand == VPackSortMigrationCheck) ||
        (request()->requestType() == rest::RequestType::PUT &&
         subCommand == VPackSortMigrationMigrate) ||
        (request()->requestType() == rest::RequestType::GET &&
         subCommand == VPackSortMigrationStatus))) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    co_return;
  }

  // We behave differently on a coordinator and on a single server,
  // dbserver or agent.
  // On a coordinator, we basically implement a trampoline to all dbservers.
  // On the other instance types, we implement the actual checking and
  // migration logic. Agents do not have to be checked since they do not
  // use VPack indexes.
  VPackBuilder result;
  Result res;
  if (!ServerState::instance()->isCoordinator()) {
    if (request()->requestType() == rest::RequestType::GET) {
      if (subCommand == VPackSortMigrationCheck) {
        res = ::analyzeVPackIndexSorting(_vocbase, result);
      } else {
        res = ::statusVPackIndexSorting(_vocbase, result);
      }
    } else {  // PUT
      res = ::migrateVPackIndexSorting(_vocbase, result);
    }
  } else {
    // Coordinators from here:
    fuerte::RestVerb verb = request()->requestType() == rest::RequestType::GET
                                ? fuerte::RestVerb::Get
                                : fuerte::RestVerb::Put;
    res = co_await ::fanOutRequests(_vocbase, verb, subCommand, result);
  }
  if (res.fail()) {
    generateError(rest::ResponseCode::SERVER_ERROR, res.errorNumber(),
                  res.errorMessage());
  } else {
    generateOk(rest::ResponseCode::OK, result.slice());
  }
  co_return;
}
