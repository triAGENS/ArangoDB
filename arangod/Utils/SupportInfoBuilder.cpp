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
/// @author Julia Puget
////////////////////////////////////////////////////////////////////////////////

#include "SupportInfoBuilder.h"

#include "Agency/AsyncAgencyComm.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/NumberOfCores.h"
#include "Basics/PhysicalMemory.h"
#include "Basics/process-utils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes/Index.h"
#include "Metrics/MetricsFeature.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Replication/ReplicationFeature.h"
#include "Rest/Version.h"
#include "RestServer/CpuUsageFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/EnvironmentFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "Statistics/ServerStatistics.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/DatabaseGuard.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"
#include "VocBase/Methods/Indexes.h"

#include <velocypack/Builder.h>
#include <absl/strings/str_replace.h>

#include "Logger/LogMacros.h"

using namespace arangodb;
using namespace arangodb::rest;
using namespace std::literals;

namespace {
network::Headers buildHeaders() {
  auto auth = AuthenticationFeature::instance();

  network::Headers headers;
  if (auth != nullptr && auth->isActive()) {
    headers.try_emplace(StaticStrings::Authorization,
                        "bearer " + auth->tokenCache().jwtToken());
  }
  return headers;
}
}  // namespace

void SupportInfoBuilder::addDatabaseInfo(VPackBuilder& result,
                                         VPackSlice infoSlice,
                                         ArangodServer& server) {
  DatabaseFeature& dbFeature = server.getFeature<DatabaseFeature>();

  std::vector<std::string> databases = methods::Databases::list(server, "");

  containers::FlatHashMap<std::string_view, uint32_t> dbViews;
  for (auto const& database : databases) {
    auto vocbase = dbFeature.lookupDatabase(database);

    LogicalView::enumerate(*vocbase,
                           [&dbViews, &database = std::as_const(database)](
                               LogicalView::ptr const& view) -> bool {
                             dbViews[database]++;

                             return true;
                           });
  }

  struct DbCollStats {
    size_t numDocColls;
    size_t numGraphColls;
    size_t numSmartColls;
    size_t numDisjointGraphs;
    VPackBuilder builder;
  };

  std::unordered_map<std::string_view, DbCollStats> visitedDatabases;
  containers::FlatHashMap<size_t, containers::FlatHashSet<std::string_view>>
      visitedColls;
  containers::FlatHashMap<size_t, size_t> collNumDocs;
  // merge all collections belonging to the database into
  // the same object, as the database might be in more
  // than one dbserver

  for (auto const dbIt : velocypack::ArrayIterator(infoSlice)) {
    std::string_view dbName = dbIt.get("name").stringView();
    if (visitedDatabases.find(dbName) == visitedDatabases.end()) {
      visitedDatabases.insert(std::make_pair(dbName, DbCollStats()));
      visitedDatabases[dbName].builder.openObject();
      visitedDatabases[dbName].builder.add(
          "n_views", VPackValue(dbViews[dbIt.get("name").stringView()]));
      visitedDatabases[dbName].builder.add(
          "single_shard", VPackValue(dbIt.get("single_shard").getBoolean()));
      visitedDatabases[dbName].builder.add("colls",
                                           VPackValue(VPackValueType::Array));
    }

    for (auto const collIt : velocypack::ArrayIterator(dbIt.get("colls"))) {
      size_t planId = collIt.get("plan_id").getUInt();

      std::string_view collName = collIt.get("name").stringView();
      if (auto const visitedCollIt = visitedColls.find(planId);
          visitedCollIt != visitedColls.end()) {
        if (visitedCollIt->second.find(collName) ==
            visitedCollIt->second.end()) {
          collNumDocs[planId] += collIt.get("n_docs").getUInt();
        }
      } else {
        collNumDocs[planId] += collIt.get("n_docs").getUInt();
        visitedDatabases[dbName].builder.add(collIt);
      }
    }

    auto const allDbValues = dbIt.value();
    visitedDatabases[dbName].numDocColls +=
        allDbValues.get("n_doc_colls").getUInt();

    visitedDatabases[dbName].numGraphColls +=
        allDbValues.get("n_graph_colls").getUInt();

    visitedDatabases[dbName].numSmartColls +=
        allDbValues.get("n_smart_colls").getUInt();

    visitedDatabases[dbName].numDisjointGraphs +=
        allDbValues.get("n_disjoint_graphs").getUInt();
  }

  for (auto& [dbName, dbInfo] : visitedDatabases) {
    if (!dbInfo.builder.isEmpty() && dbInfo.builder.isOpenArray()) {
      dbInfo.builder.close();
    }
    if (!dbInfo.builder.isEmpty() && !dbInfo.builder.isClosed()) {
      dbInfo.builder.close();
    }
    result.openObject();
    result.add("n_doc_colls", VPackValue(dbInfo.numDocColls));
    result.add("n_graph_colls", VPackValue(dbInfo.numGraphColls));
    result.add("n_smart_colls", VPackValue(dbInfo.numSmartColls));
    result.add("n_disjoint_graphs", VPackValue(dbInfo.numDisjointGraphs));

    for (auto const dbIt : velocypack::ObjectIterator(dbInfo.builder.slice())) {
      std::string_view key = dbIt.key.stringView();
      auto const value = dbIt.value;
      if (key == "colls") {
        result.add("colls", VPackValue(VPackValueType::Array));
        for (auto const collIt : velocypack::ArrayIterator(value)) {
          result.openObject();
          for (auto const collIt2 : velocypack::ObjectIterator(collIt)) {
            std::string_view key2 = collIt2.key.stringView();
            auto const value2 = collIt2.value;
            if (key2 == "n_docs") {
              auto foundIt = collNumDocs.find(collIt.get("plan_id").getUInt());
              if (foundIt != collNumDocs.end()) {
                result.add(key2, VPackValue(foundIt->second));
              }
            } else if (key2 != "name") {
              result.add(key2, value2);
            }
          }
          result.close();
        }
        result.close();

      } else {
        result.add(key, value);
      }
    }
    result.close();
  }
}

// if it's a telemetrics request, keys can only have lowercase letters and
// underscores
void SupportInfoBuilder::normalizeKeyForTelemetrics(std::string& key) {
  key = arangodb::basics::StringUtils::tolower(key.data());
  key = absl::StrReplaceAll(std::move(key), {{".", "_"}, {"-", "_"}});
}

void SupportInfoBuilder::buildInfoMessage(VPackBuilder& result,
                                          std::string const& dbName,
                                          ArangodServer& server, bool isLocal,
                                          bool isTelemetricsReq) {
  bool isSingleServer = ServerState::instance()->isSingleServer();
  auto const serverId = ServerIdFeature::getId().id();
  // used for all types of responses
  VPackBuilder hostInfo;

  buildHostInfo(hostInfo, server, isTelemetricsReq);

  std::string timeString;
  LogTimeFormats::writeTime(timeString,
                            LogTimeFormats::TimeFormat::UTCDateString,
                            std::chrono::system_clock::now());

  bool const isActiveFailover =
      server.getFeature<ReplicationFeature>().isActiveFailoverEnabled();
  bool fanout =
      (ServerState::instance()->isCoordinator() || isActiveFailover) &&
      !isLocal;

  result.openObject();

  if (isSingleServer && !isActiveFailover) {
    if (!isTelemetricsReq) {
      result.add("id", VPackValue(serverId));
    }
    result.add("deployment", VPackValue(VPackValueType::Object));
    std::string persistedId;
    if (isTelemetricsReq && ServerState::instance()->hasPersistedId()) {
      persistedId = ServerState::instance()->getPersistedId();
      normalizeKeyForTelemetrics(persistedId);
    } else {
      persistedId = "single_" + std::to_string(serverId);
    }
    result.add("persistedId", VPackValue(persistedId));
    if (isTelemetricsReq) {
      result.add("id", VPackValue("single_" + std::to_string(serverId)));
    }
    result.add("type", VPackValue("single"));
#ifdef USE_ENTERPRISE
    result.add("license", VPackValue("enterprise"));
#else
    result.add("license", VPackValue("community"));
#endif
    if (isTelemetricsReq) {
      // it's single server, but we maintain the format the same as cluster
      result.add("servers", VPackValue(VPackValueType::Array));
      result.openObject();
      result.add("instance", hostInfo.slice());
      result.close();
      result.close();  // servers
    } else {
      result.add(persistedId, hostInfo.slice());
    }
    result.close();  // deployment
    result.add("date", VPackValue(timeString));
    VPackBuilder serverInfo;
    buildDbServerDataStoredInfo(serverInfo, server);
    result.add("databases", VPackValue(VPackValueType::Array));
    addDatabaseInfo(result, serverInfo.slice().get("databases"), server);
    result.close();
  } else {
    // cluster or active failover
    if (fanout) {
      // cluster coordinator or active failover case, fan out to other servers!
      result.add("deployment", VPackValue(VPackValueType::Object));
#ifdef USE_ENTERPRISE
      result.add("license", VPackValue("enterprise"));
#else
      result.add("license", VPackValue("community"));
#endif
      if (isTelemetricsReq && ServerState::instance()->hasPersistedId()) {
        result.add("persistedId",
                   VPackValue(arangodb::basics::StringUtils::tolower(
                       ServerState::instance()->getPersistedId())));
      }
      if (isActiveFailover) {
        TRI_ASSERT(!ServerState::instance()->isCoordinator());
        result.add("type", VPackValue("active_failover"));
      } else {
        TRI_ASSERT(ServerState::instance()->isCoordinator());
        result.add("type", VPackValue("cluster"));
      }

      // build results for all servers
      result.add("servers", VPackValue(VPackValueType::Array));
      // we come first!
      auto serverId = ServerState::instance()->getId();
      if (isTelemetricsReq) {
        serverId = arangodb::basics::StringUtils::tolower(serverId.data());
        normalizeKeyForTelemetrics(serverId);
      }
      result.openObject();
      result.add(serverId, hostInfo.slice());
      result.close();

      // now all other servers
      NetworkFeature const& nf = server.getFeature<NetworkFeature>();
      network::ConnectionPool* pool = nf.pool();
      if (pool == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
      }

      std::vector<network::FutureRes> futures;

      network::RequestOptions options;
      options.timeout = network::Timeout(30.0);
      options.database = dbName;
      options.param("local", "true");
      options.param("support", "true");

      size_t coordinators = 0;
      size_t dbServers = 0;
      size_t agents = 0;

      std::vector<std::string> dbServerNames;
      ClusterInfo& ci = server.getFeature<ClusterFeature>().clusterInfo();
      for (auto const& server : ci.getServers()) {
        if (server.first.starts_with("CRDN")) {
          ++coordinators;
        } else if (server.first.starts_with("PRMR")) {
          dbServerNames.emplace_back(server.first);
          ++dbServers;
        } else if (server.first.starts_with("SNGL")) {
          // SNGL counts as DB server here
          TRI_ASSERT(isActiveFailover);
          ++dbServers;
        }
        if (server.first == ServerState::instance()->getId()) {
          // ourselves!
          continue;
        }

        std::string reqUrl =
            isTelemetricsReq ? "/_admin/server-info" : "/_admin/support-info";
        auto f = network::sendRequestRetry(
            pool, "server:" + server.first, fuerte::RestVerb::Get, reqUrl,
            VPackBuffer<uint8_t>{}, options, ::buildHeaders());
        futures.emplace_back(std::move(f));
      }

      VPackBuilder dbInfoBuilder;
      if (!futures.empty()) {
        auto responses = futures::collectAll(futures).get();
        for (auto const& it : responses) {
          auto& resp = it.get();
          auto res = resp.combinedResult();
          if (res.fail()) {
            LOG_TOPIC("4800b", WARN, Logger::STATISTICS)
                << "Failed to get server info: " << res.errorMessage();
          } else {
            auto slice = resp.slice();
            // copy results from other server
            if (slice.isObject()) {
              std::string hostId =
                  basics::StringUtils::replace(resp.destination, "server:", "");
              if (isTelemetricsReq) {
                normalizeKeyForTelemetrics(hostId);
              }
              result.openObject();
              result.add(hostId, slice.get("host"));
              result.close();
              if (isTelemetricsReq) {
                auto const databasesSlice = slice.get("databases");
                if (!databasesSlice.isNone()) {
                  dbInfoBuilder.add(slice.get("databases"));
                }
              }
            }
          }
        }
      }

      result.close();  // servers

      auto manager = AsyncAgencyCommManager::INSTANCE.get();
      if (manager != nullptr) {
        agents = manager->endpoints().size();
      }

      result.add("agents", VPackValue(agents));
      result.add("coordinators", VPackValue(coordinators));
      result.add("db_servers", VPackValue(dbServers));

      if (ServerState::instance()->isCoordinator()) {
        result.add(VPackValue("shards_statistics"));
        ci.getShardStatisticsGlobal("", result);
      }
      result.close();  // deployment
      result.add("date", VPackValue(timeString));

      if (isTelemetricsReq && !dbInfoBuilder.isEmpty()) {
        result.add("databases", VPackValue(VPackValueType::Array));
        addDatabaseInfo(result, dbInfoBuilder.slice(), server);
        result.close();
      }

    } else {
      // DB server or other coordinator
      result.add("host", hostInfo.slice());
      if (isTelemetricsReq && !ServerState::instance()->isCoordinator()) {
        VPackBuilder serverInfo;
        buildDbServerDataStoredInfo(serverInfo, server);
        result.add("databases", serverInfo.slice().get("databases"));
      }
    }
  }
  result.close();
}

void SupportInfoBuilder::buildHostInfo(VPackBuilder& result,
                                       ArangodServer& server,
                                       bool isTelemetricsReq) {
  bool const isActiveFailover =
      server.getFeature<ReplicationFeature>().isActiveFailoverEnabled();

  result.openObject();

  if (ServerState::instance()->isRunningInCluster() || isActiveFailover) {
    auto serverId = ServerState::instance()->getId();
    if (isTelemetricsReq) {
      normalizeKeyForTelemetrics(serverId);
    }
    result.add("id", VPackValue(serverId));
    result.add("alias", VPackValue(ServerState::instance()->getShortName()));
    result.add("endpoint", VPackValue(ServerState::instance()->getEndpoint()));
  }

  result.add("role", VPackValue(ServerState::roleToString(
                         ServerState::instance()->getRole())));
  result.add("maintenance",
             VPackValue(ServerState::instance()->isStartupOrMaintenance()));
  result.add("read_only", VPackValue(ServerState::instance()->readOnly()));

  result.add("version", VPackValue(ARANGODB_VERSION));
  result.add("build", VPackValue(Version::getBuildRepository()));

  EnvironmentFeature const& ef = server.getFeature<EnvironmentFeature>();
  result.add("os", VPackValue(ef.operatingSystem()));
  result.add("platform", VPackValue(Version::getPlatform()));

  result.add("phys_mem", VPackValue(VPackValueType::Object));
  result.add("value", VPackValue(PhysicalMemory::getValue()));
  result.add("overridden", VPackValue(PhysicalMemory::overridden()));
  result.close();  // physical memory

  result.add("n_cores", VPackValue(VPackValueType::Object));
  result.add("value", VPackValue(NumberOfCores::getValue()));
  result.add("overridden", VPackValue(NumberOfCores::overridden()));
  result.close();  // number of cores

  result.add("process_stats", VPackValue(VPackValueType::Object));
  ServerStatistics const& serverInfo =
      server.getFeature<metrics::MetricsFeature>().serverStatistics();
  result.add("process_uptime", VPackValue(serverInfo.uptime()));

  ProcessInfo info = TRI_ProcessInfoSelf();
  result.add("n_threads", VPackValue(info._numberThreads));
  result.add("virtual_size", VPackValue(info._virtualSize));
  result.add("resident_set_size", VPackValue(info._residentSize));
  result.close();  // processStats

  CpuUsageFeature& cpuUsage = server.getFeature<CpuUsageFeature>();
  if (cpuUsage.isEnabled() && !isTelemetricsReq) {
    auto snapshot = cpuUsage.snapshot();
    result.add("cpu_stats", VPackValue(VPackValueType::Object));
    result.add("user_percent", VPackValue(snapshot.userPercent()));
    result.add("system_percent", VPackValue(snapshot.systemPercent()));
    result.add("idle_percent", VPackValue(snapshot.idlePercent()));
    result.add("iowait_percent", VPackValue(snapshot.iowaitPercent()));
    result.close();  // cpustats
  }

  if (!ServerState::instance()->isCoordinator()) {
    result.add("engine_stats", VPackValue(VPackValueType::Object));
    VPackBuilder stats;
    StorageEngine& engine = server.getFeature<EngineSelectorFeature>().engine();
    engine.getStatistics(stats);

    auto names = {
        // edge cache
        "cache.limit",
        "cache.allocated",
        // sizes
        "rocksdb.estimate-num-keys",
        "rocksdb.estimate-live-data-size",
        "rocksdb.live-sst-files-size",
        // block cache
        "rocksdb.block-cache-capacity",
        "rocksdb.block-cache-usage",
        // disk
        "rocksdb.free-disk-space",
        "rocksdb.total-disk-space",
    };
    for (auto& name : names) {
      std::string newName(name);
      if (isTelemetricsReq) {
        normalizeKeyForTelemetrics(newName);
      }
      if (auto slice = stats.slice().get(name); !slice.isNone()) {
        result.add(newName, slice);
      } else if (isTelemetricsReq) {
        result.add(newName, VPackValue(0));
      }
    }
    result.close();  // engineStats
  }

  result.close();
}

void SupportInfoBuilder::buildDbServerDataStoredInfo(
    velocypack::Builder& result, ArangodServer& server) {
  DatabaseFeature& dbFeature = server.getFeature<DatabaseFeature>();
  std::vector<std::string> databases = methods::Databases::list(server, "");

  ExecContextSuperuserScope ctxScope;

  result.openObject();

  result.add("databases", VPackValue(VPackValueType::Array));
  for (auto const& database : databases) {
    auto vocbase = dbFeature.lookupDatabase(database);

    result.openObject();
    result.add("name", database);

    size_t numDocColls = 0;
    size_t numGraphColls = 0;
    size_t numSmartColls = 0;
    size_t numDisjointGraphs = 0;

    result.add("colls", VPackValue(VPackValueType::Array));

    containers::FlatHashSet<size_t> collsAlreadyVisited;

    DatabaseGuard guard(dbFeature, database);
    methods::Collections::enumerate(
        &guard.database(), [&](std::shared_ptr<LogicalCollection> const& coll) {
          result.openObject();
          size_t numShards = coll->numberOfShards();
          result.add("n_shards", VPackValue(numShards));
          result.add("rep_factor", VPackValue(coll->replicationFactor()));

          auto ctx = transaction::StandaloneContext::Create(*vocbase);

          auto& collName = coll->name();
          result.add("name", VPackValue(collName));
          size_t planId = coll->planId().id();
          result.add("plan_id", VPackValue(planId));

          SingleCollectionTransaction trx(ctx, collName,
                                          AccessMode::Type::READ);

          Result res = trx.begin();

          if (res.ok()) {
            OperationOptions options(ExecContext::current());

            OperationResult opResult =
                trx.count(collName, transaction::CountType::Normal, options);
            std::ignore = trx.finish(opResult.result);
            if (opResult.fail()) {
              LOG_TOPIC("8ae00", WARN, Logger::STATISTICS)
                  << "Failed to get number of documents: "
                  << res.errorMessage();
            } else {
              result.add("n_docs", VPackSlice(opResult.buffer->data()));
            }

          } else {
            LOG_TOPIC("e7497", WARN, Logger::STATISTICS)
                << "Failed to begin transaction for getting number of "
                   "documents: "
                << res.errorMessage();
          }

          if (collsAlreadyVisited.find(planId) == collsAlreadyVisited.end()) {
            auto collType = coll->type();
            if (collType == TRI_COL_TYPE_DOCUMENT) {
              numDocColls++;
              result.add("type", VPackValue("document"));
            } else if (collType == TRI_COL_TYPE_EDGE) {
              result.add("type", VPackValue("edge"));
              numGraphColls++;
            } else {
              result.add("type", VPackValue("unknown"));
            }
            if (coll->isSmart()) {
              numSmartColls++;
              result.add("smart_graph", VPackValue(true));
            } else {
              result.add("smart_graph", VPackValue(false));
            }
            if (coll->isDisjoint()) {
              numDisjointGraphs++;
              result.add("disjoint", VPackValue(true));
            } else {
              result.add("disjoint", VPackValue(false));
            }

            //  std::unordered_map<std::string, size_t> idxTypesToAmounts;
            containers::FlatHashMap<std::string, size_t> idxTypesToAmounts;

            auto flags = Index::makeFlags(Index::Serialize::Estimates,
                                          Index::Serialize::Figures);
            std::vector<std::string_view> idxTypes = {
                "edge",      "geo",        "hash",      "fulltext", "inverted",
                "no-access", "persistent", "iresearch", "skiplist", "ttl",
                "zkd",       "primary",    "unknown"};
            for (auto const& type : idxTypes) {
              idxTypesToAmounts.try_emplace(type, 0);
            }

            VPackBuilder output;
            std::ignore =
                methods::Indexes::getAll(coll.get(), flags, false, output);
            velocypack::Slice outSlice = output.slice();

            result.add("idxs", VPackValue(VPackValueType::Array));
            for (auto const it : velocypack::ArrayIterator(outSlice)) {
              result.openObject();

              if (auto figures = it.get("figures"); !figures.isNone()) {
                auto idxSlice = figures.get("memory");
                uint64_t memUsage = 0;
                if (!idxSlice.isNone()) {
                  memUsage = idxSlice.getUInt();
                }
                result.add("mem", VPackValue(memUsage));
                idxSlice = figures.get("cache_in_use");
                bool cacheInUse = false;
                if (!idxSlice.isNone()) {
                  cacheInUse = idxSlice.getBoolean();
                }
                uint64_t cacheSize = 0;
                uint64_t cacheUsage = 0;
                if (cacheInUse) {
                  cacheSize = figures.get("cache_size").getUInt();
                  cacheUsage = figures.get("cache_usage").getUInt();
                }
                result.add("cache_size", VPackValue(cacheSize));
                result.add("cache_usage", VPackValue(cacheUsage));
              }

              auto idxType = it.get("type").stringView();
              result.add("type", VPackValue(idxType));
              bool isSparse = it.get("sparse").getBoolean();
              bool isUnique = it.get("unique").getBoolean();
              result.add("sparse", VPackValue(isSparse));
              result.add("unique", VPackValue(isUnique));
              idxTypesToAmounts[idxType]++;

              result.close();
            }
            result.close();
            collsAlreadyVisited.insert(planId);
            for (auto const& [type, amount] : idxTypesToAmounts) {
              if (type == "no-access") {
                result.add("n_no_access", VPackValue(amount));
              } else {
                result.add({"n_" + type}, VPackValue(amount));
              }
            }
          }

          result.close();
        });

    result.close();

    result.add("single_shard", VPackValue(vocbase->isOneShard()));
    result.add("n_doc_colls", VPackValue(numDocColls));
    result.add("n_graph_colls", VPackValue(numGraphColls));
    result.add("n_smart_colls", VPackValue(numSmartColls));
    result.add("n_disjoint_graphs", VPackValue(numDisjointGraphs));

    result.close();
  }
  result.close();

  result.close();
}
