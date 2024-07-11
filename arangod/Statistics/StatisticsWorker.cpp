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
/// @author Manuel Baesler
////////////////////////////////////////////////////////////////////////////////

#include "StatisticsWorker.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryOptions.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Basics/PhysicalMemory.h"
#include "Basics/StaticStrings.h"
#include "Basics/process-utils.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Random/RandomGenerator.h"
#include "Metrics/Counter.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/CpuUsageFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/TtlFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "Statistics/ConnectionStatistics.h"
#include "Statistics/RequestStatistics.h"
#include "Statistics/ServerStatistics.h"
#include "Statistics/StatisticsFeature.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Indexes.h"

#include <velocypack/Exception.h>
#include <velocypack/Iterator.h>

#include <string_view>

namespace {
constexpr std::string_view garbageCollectionQuery(
    "FOR s in @@collection FILTER s.time < @start RETURN s._key");

constexpr std::string_view lastEntryQuery(
    "FOR s in @@collection FILTER s.time >= @start SORT s.time DESC LIMIT 1 "
    "RETURN s");
constexpr std::string_view filteredLastEntryQuery(
    "FOR s in @@collection FILTER s.time >= @start FILTER s.clusterId == "
    "@clusterId SORT s.time DESC LIMIT 1 RETURN s");

constexpr std::string_view fifteenMinuteQuery(
    "FOR s in _statistics FILTER s.time >= @start SORT s.time RETURN s");

constexpr std::string_view filteredFifteenMinuteQuery(
    "FOR s in _statistics FILTER s.time >= @start FILTER s.clusterId == "
    "@clusterId SORT s.time RETURN s");

double extractNumber(VPackSlice slice, std::string_view attribute) {
  if (!slice.isObject()) {
    return 0.0;
  }
  slice = slice.get(attribute);
  if (!slice.isNumber()) {
    return 0.0;
  }
  return slice.getNumber<double>();
}

}  // namespace

using namespace arangodb;
using namespace arangodb::statistics;

StatisticsWorker::StatisticsWorker(TRI_vocbase_t& vocbase)
    : ServerThread<ArangodServer>(vocbase.server(), "StatisticsWorker"),
      _gcTask(GC_STATS),
      _vocbase(vocbase) {
  _bytesSentDistribution.openArray();

  for (auto const& val : BytesSentDistributionCuts) {
    _bytesSentDistribution.add(VPackValue(val));
  }

  _bytesSentDistribution.close();

  _bytesReceivedDistribution.openArray();

  for (auto const& val : BytesReceivedDistributionCuts) {
    _bytesReceivedDistribution.add(VPackValue(val));
  }

  _bytesReceivedDistribution.close();

  _requestTimeDistribution.openArray();

  for (auto const& val : RequestTimeDistributionCuts) {
    _requestTimeDistribution.add(VPackValue(val));
  }

  _requestTimeDistribution.close();

  _bindVars = std::make_shared<VPackBuilder>();
}

void StatisticsWorker::collectGarbage() {
  // use _gcTask to separate the different garbage collection operations,
  // and not have them execute all at once sequentially (potential load spike),
  // but only one task at a time. this should spread the load more evenly
  auto time = TRI_microtime();

  try {
    if (_gcTask == GC_STATS) {
      collectGarbage(StaticStrings::StatisticsCollection,
                     time - 3600.0);  // 1 hour
      _gcTask = GC_STATS_RAW;
    } else if (_gcTask == GC_STATS_RAW) {
      collectGarbage(StaticStrings::StatisticsRawCollection,
                     time - 3600.0);  // 1 hour
      _gcTask = GC_STATS_15;
    } else if (_gcTask == GC_STATS_15) {
      collectGarbage(StaticStrings::Statistics15Collection,
                     time - 30.0 * 86400.0);  // 30 days
      _gcTask = GC_STATS;
    }
  } catch (basics::Exception const& ex) {
    if (ex.code() != TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
      // if the underlying collection does not exist, it does not matter
      // that the garbage collection query failed
      throw;
    }
  }
}

void StatisticsWorker::collectGarbage(std::string const& name,
                                      double start) const {
  auto bindVars = _bindVars.get();

  bindVars->clear();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(name));
  bindVars->add("start", VPackValue(start));
  bindVars->close();

  auto origin =
      transaction::OperationOriginInternal{"statistics garbage collection"};

  aql::QueryOptions options;
  options.cache = false;
  options.skipAudit = true;
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      _vocbase, origin, aql::QueryString(::garbageCollectionQuery), _bindVars,
      std::move(options));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice keysToRemove = queryResult.data->slice();
  OperationOptions opOptions;

  opOptions.ignoreRevs = true;
  opOptions.waitForSync = false;
  opOptions.silent = true;

  auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
  SingleCollectionTransaction trx(std::move(ctx), name,
                                  AccessMode::Type::WRITE);

  Result res = trx.begin();

  if (!res.ok()) {
    return;
  }

  OperationResult result = trx.remove(name, keysToRemove, opOptions);

  res = trx.finish(result.result);

  if (res.fail()) {
    LOG_TOPIC("14fa9", WARN, Logger::STATISTICS)
        << "removing outdated statistics failed: " << res.errorMessage();
  }
}

void StatisticsWorker::historian() {
  try {
    double now = TRI_microtime();
    // only query the previously stored statistics value once on our first
    // iteration
    if (_lastStoredValue.slice().isNone()) {
      auto prevRawBuilder = lastEntry(StaticStrings::StatisticsRawCollection,
                                      now - 2.0 * INTERVAL);

      VPackSlice prevRaw = prevRawBuilder->slice();
      if (prevRaw.isArray() && prevRaw.length()) {
        _lastStoredValue.add(prevRaw.at(0).resolveExternals());
      }
    }

    _rawBuilder.clear();
    generateRawStatistics(_rawBuilder, now);

    saveSlice(_rawBuilder.slice(), StaticStrings::StatisticsRawCollection);

    // create the per-seconds statistics
    if (_lastStoredValue.slice().isObject()) {
      _tempBuilder.clear();
      computePerSeconds(_tempBuilder, _rawBuilder.slice(),
                        _lastStoredValue.slice());
      VPackSlice perSecs = _tempBuilder.slice();

      if (perSecs.length()) {
        saveSlice(perSecs, StaticStrings::StatisticsCollection);
      }
    }

    // remember what we stored last, so we don't have to re-query it on the next
    // iteration
    _lastStoredValue.clear();
    _lastStoredValue.add(_rawBuilder.slice());
  } catch (...) {
  }
}

void StatisticsWorker::historianAverage() {
  try {
    double now = TRI_microtime();

    std::shared_ptr<arangodb::velocypack::Builder> prev15Builder = lastEntry(
        StaticStrings::Statistics15Collection, now - 2.0 * HISTORY_INTERVAL);
    VPackSlice prev15 = prev15Builder->slice();

    double start;
    if (prev15.isArray() && prev15.length()) {
      VPackSlice slice = prev15.at(0).resolveExternals();
      start = slice.get("time").getNumber<double>();
    } else {
      start = now - HISTORY_INTERVAL;
    }

    _tempBuilder.clear();
    compute15Minute(_tempBuilder, start);
    VPackSlice stat15 = _tempBuilder.slice();

    if (stat15.length()) {
      saveSlice(stat15, StaticStrings::Statistics15Collection);
    }
  } catch (velocypack::Exception const& ex) {
    LOG_TOPIC("1c429", DEBUG, Logger::STATISTICS)
        << "vpack exception in historian average: " << ex.what();
  } catch (basics::Exception const& ex) {
    LOG_TOPIC("40480", DEBUG, Logger::STATISTICS)
        << "exception in historian average: " << ex.what();
  } catch (...) {
    LOG_TOPIC("570d6", DEBUG, Logger::STATISTICS)
        << "unknown exception in historian average";
  }
}

std::shared_ptr<arangodb::velocypack::Builder> StatisticsWorker::lastEntry(
    std::string const& collectionName, double start) const {
  auto bindVars = _bindVars.get();

  bindVars->clear();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(collectionName));
  bindVars->add("start", VPackValue(start));

  if (!_clusterId.empty()) {
    bindVars->add("clusterId", VPackValue(_clusterId));
  }

  bindVars->close();

  auto origin =
      transaction::OperationOriginInternal{"fetching last statistics entry"};

  aql::QueryOptions options;
  options.cache = false;
  options.skipAudit = true;
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      _vocbase, origin,
      aql::QueryString(_clusterId.empty() ? ::lastEntryQuery
                                          : ::filteredLastEntryQuery),
      _bindVars, std::move(options));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  return queryResult.data;
}

void StatisticsWorker::compute15Minute(VPackBuilder& builder, double start) {
  auto bindVars = _bindVars.get();

  bindVars->clear();
  bindVars->openObject();
  bindVars->add("start", VPackValue(start));

  if (!_clusterId.empty()) {
    bindVars->add("clusterId", VPackValue(_clusterId));
  }

  bindVars->close();

  auto origin =
      transaction::OperationOriginInternal{"computing statistics 15m average"};

  aql::QueryOptions options;
  options.cache = false;
  options.skipAudit = true;
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      _vocbase, origin,
      aql::QueryString(_clusterId.empty() ? ::fifteenMinuteQuery
                                          : ::filteredFifteenMinuteQuery),
      _bindVars, std::move(options));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice result = queryResult.data->slice();
  uint64_t count = result.length();

  builder.clear();
  if (count == 0) {
    builder.openObject();
    builder.close();
    return;
  }

  VPackSlice last = result.at(count - 1).resolveExternals();

  double serverV8available = 0, serverV8busy = 0, serverV8dirty = 0,
         serverV8free = 0, serverV8max = 0, serverThreadsRunning = 0,
         serverThreadsWorking = 0, serverThreadsBlocked = 0,
         serverThreadsQueued = 0,

         systemMinorPageFaultsPerSecond = 0, systemMajorPageFaultsPerSecond = 0,
         systemUserTimePerSecond = 0, systemSystemTimePerSecond = 0,
         systemResidentSize = 0, systemVirtualSize = 0,
         systemNumberOfThreads = 0,

         httpRequestsTotalPerSecond = 0, httpRequestsAsyncPerSecond = 0,
         httpRequestsGetPerSecond = 0, httpRequestsHeadPerSecond = 0,
         httpRequestsPostPerSecond = 0, httpRequestsPutPerSecond = 0,
         httpRequestsPatchPerSecond = 0, httpRequestsDeletePerSecond = 0,
         httpRequestsOptionsPerSecond = 0, httpRequestsOtherPerSecond = 0,

         clientHttpConnections = 0, clientBytesSentPerSecond = 0,
         clientBytesReceivedPerSecond = 0, clientAvgTotalTime = 0,
         clientAvgRequestTime = 0, clientAvgQueueTime = 0, clientAvgIoTime = 0;

  for (auto const& vs : VPackArrayIterator(result)) {
    VPackSlice const values = vs.resolveExternals();

    if (!values.isObject()) {
      // oops
      continue;
    }

    VPackSlice server = values.get("server");

    try {
      // in an environment that is mixing 3.4 and previous versions, the
      // following attributes may not be present. we don't want the statistics
      // to give up in this case, but simply ignore these errors
      try {
        VPackSlice v8Contexts = server.get("v8Context");
        serverV8available += ::extractNumber(v8Contexts, "availablePerSecond");
        serverV8busy += ::extractNumber(v8Contexts, "busyPerSecond");
        serverV8dirty += ::extractNumber(v8Contexts, "dirtyPerSecond");
        serverV8free += ::extractNumber(v8Contexts, "freePerSecond");
        serverV8max += ::extractNumber(v8Contexts, "maxPerSecond");
      } catch (...) {
        // if attribute "v8Context" is not present, simply do not count
      }

      try {
        VPackSlice threads = server.get("threads");
        serverThreadsRunning += ::extractNumber(threads, "runningPerSecond");
        serverThreadsWorking += ::extractNumber(threads, "workingPerSecond");
        serverThreadsBlocked += ::extractNumber(threads, "blockedPerSecond");
        serverThreadsQueued += ::extractNumber(threads, "queuedPerSecond");
      } catch (...) {
        // if attribute "threads" is not present, simply do not count
      }

      try {
        VPackSlice system = values.get("system");
        systemMinorPageFaultsPerSecond +=
            ::extractNumber(system, "minorPageFaultsPerSecond");
        systemMajorPageFaultsPerSecond +=
            ::extractNumber(system, "majorPageFaultsPerSecond");
        systemUserTimePerSecond += ::extractNumber(system, "userTimePerSecond");
        systemSystemTimePerSecond +=
            ::extractNumber(system, "systemTimePerSecond");
        systemResidentSize += ::extractNumber(system, "residentSize");
        systemVirtualSize += ::extractNumber(system, "virtualSize");
        systemNumberOfThreads += ::extractNumber(system, "numberOfThreads");
      } catch (...) {
        // if attribute "system" is not present, simply do not count
      }

      try {
        VPackSlice http = values.get("http");
        httpRequestsTotalPerSecond +=
            ::extractNumber(http, "requestsTotalPerSecond");
        httpRequestsAsyncPerSecond +=
            ::extractNumber(http, "requestsAsyncPerSecond");
        httpRequestsGetPerSecond +=
            ::extractNumber(http, "requestsGetPerSecond");
        httpRequestsHeadPerSecond +=
            ::extractNumber(http, "requestsHeadPerSecond");
        httpRequestsPostPerSecond +=
            ::extractNumber(http, "requestsPostPerSecond");
        httpRequestsPutPerSecond +=
            ::extractNumber(http, "requestsPutPerSecond");
        httpRequestsPatchPerSecond +=
            ::extractNumber(http, "requestsPatchPerSecond");
        httpRequestsDeletePerSecond +=
            ::extractNumber(http, "requestsDeletePerSecond");
        httpRequestsOptionsPerSecond +=
            ::extractNumber(http, "requestsOptionsPerSecond");
        httpRequestsOtherPerSecond +=
            ::extractNumber(http, "requestsOtherPerSecond");
      } catch (...) {
        // if attribute "http" is not present, simply do not count
      }

      try {
        VPackSlice client = values.get("client");
        clientHttpConnections += ::extractNumber(client, "httpConnections");
        clientBytesSentPerSecond +=
            ::extractNumber(client, "bytesSentPerSecond");
        clientBytesReceivedPerSecond +=
            ::extractNumber(client, "bytesReceivedPerSecond");
        clientAvgTotalTime += ::extractNumber(client, "avgTotalTime");
        clientAvgRequestTime += ::extractNumber(client, "avgRequestTime");
        clientAvgQueueTime += ::extractNumber(client, "avgQueueTime");
        clientAvgIoTime += ::extractNumber(client, "avgIoTime");
      } catch (...) {
        // if attribute "client" is not present, simply do not count
      }
    } catch (std::exception const& ex) {
      // should almost never happen now
      LOG_TOPIC("e4102", WARN, Logger::STATISTICS)
          << "caught exception during statistics processing: " << ex.what();
    }
  }

  TRI_ASSERT(count > 0);

  serverV8available /= count;
  serverV8busy /= count;
  serverV8dirty /= count;
  serverV8free /= count;
  serverV8max /= count;

  serverThreadsRunning /= count;
  serverThreadsWorking /= count;
  serverThreadsBlocked /= count;
  serverThreadsQueued /= count;

  systemMinorPageFaultsPerSecond /= count;
  systemMajorPageFaultsPerSecond /= count;
  systemUserTimePerSecond /= count;
  systemSystemTimePerSecond /= count;
  systemResidentSize /= count;
  systemVirtualSize /= count;
  systemNumberOfThreads /= count;

  httpRequestsTotalPerSecond /= count;
  httpRequestsAsyncPerSecond /= count;
  httpRequestsGetPerSecond /= count;
  httpRequestsHeadPerSecond /= count;
  httpRequestsPostPerSecond /= count;
  httpRequestsPutPerSecond /= count;
  httpRequestsPatchPerSecond /= count;
  httpRequestsDeletePerSecond /= count;
  httpRequestsOptionsPerSecond /= count;
  httpRequestsOtherPerSecond /= count;

  clientHttpConnections /= count;
  clientBytesSentPerSecond /= count;
  clientBytesReceivedPerSecond /= count;
  clientAvgTotalTime /= count;
  clientAvgRequestTime /= count;
  clientAvgQueueTime /= count;
  clientAvgIoTime /= count;

  builder.openObject();

  builder.add("time", last.get("time"));

  if (!_clusterId.empty()) {
    builder.add("clusterId", VPackValue(_clusterId));
  }

  builder.add("server", VPackValue(VPackValueType::Object));
  builder.add("physicalMemory", last.get("server").get("physicalMemory"));
  builder.add("uptime", last.get("server").get("uptime"));

  builder.add("v8Context", VPackValue(VPackValueType::Object));
  builder.add("availablePerSecond", VPackValue(serverV8available));
  builder.add("busyPerSecond", VPackValue(serverV8busy));
  builder.add("dirtyPerSecond", VPackValue(serverV8dirty));
  builder.add("freePerSecond", VPackValue(serverV8free));
  builder.add("maxPerSecond", VPackValue(serverV8max));
  builder.close();

  builder.add("threads", VPackValue(VPackValueType::Object));
  builder.add("runningPerSecond", VPackValue(serverThreadsRunning));
  builder.add("workingPerSecond", VPackValue(serverThreadsWorking));
  builder.add("blockedPerSecond", VPackValue(serverThreadsBlocked));
  builder.add("queuedPerSecond", VPackValue(serverThreadsQueued));
  builder.close();
  builder.close();

  builder.add("system", VPackValue(VPackValueType::Object));
  builder.add("minorPageFaultsPerSecond",
              VPackValue(systemMinorPageFaultsPerSecond));
  builder.add("majorPageFaultsPerSecond",
              VPackValue(systemMajorPageFaultsPerSecond));
  builder.add("userTimePerSecond", VPackValue(systemUserTimePerSecond));
  builder.add("systemTimePerSecond", VPackValue(systemSystemTimePerSecond));
  builder.add("residentSize", VPackValue(systemResidentSize));
  builder.add("virtualSize", VPackValue(systemVirtualSize));
  builder.add("numberOfThreads", VPackValue(systemNumberOfThreads));
  builder.close();

  builder.add("http", VPackValue(VPackValueType::Object));
  builder.add("requestsTotalPerSecond", VPackValue(httpRequestsTotalPerSecond));
  builder.add("requestsAsyncPerSecond", VPackValue(httpRequestsAsyncPerSecond));
  builder.add("requestsGetPerSecond", VPackValue(httpRequestsGetPerSecond));
  builder.add("requestsHeadPerSecond", VPackValue(httpRequestsHeadPerSecond));
  builder.add("requestsPostPerSecond", VPackValue(httpRequestsPostPerSecond));
  builder.add("requestsPutPerSecond", VPackValue(httpRequestsPutPerSecond));
  builder.add("requestsPatchPerSecond", VPackValue(httpRequestsPatchPerSecond));
  builder.add("requestsDeletePerSecond",
              VPackValue(httpRequestsDeletePerSecond));
  builder.add("requestsOptionsPerSecond",
              VPackValue(httpRequestsOptionsPerSecond));
  builder.add("requestsOtherPerSecond", VPackValue(httpRequestsOtherPerSecond));
  builder.close();

  builder.add("client", VPackValue(VPackValueType::Object));
  builder.add("httpConnections", VPackValue(clientHttpConnections));
  builder.add("bytesSentPerSecond", VPackValue(clientBytesSentPerSecond));
  builder.add("bytesReceivedPerSecond",
              VPackValue(clientBytesReceivedPerSecond));
  builder.add("avgTotalTime", VPackValue(clientAvgTotalTime));
  builder.add("avgRequestTime", VPackValue(clientAvgRequestTime));
  builder.add("avgQueueTime", VPackValue(clientAvgQueueTime));
  builder.add("avgIoTime", VPackValue(clientAvgIoTime));
  builder.close();

  builder.close();
}

void StatisticsWorker::computePerSeconds(VPackBuilder& result,
                                         VPackSlice current, VPackSlice prev) {
  result.clear();
  result.openObject();

  if (prev.get("time").getNumber<double>() + INTERVAL * 1.5 <
      current.get("time").getNumber<double>()) {
    result.close();
    return;
  }

  if (prev.get("server").get("uptime").getNumber<double>() >
      current.get("server").get("uptime").getNumber<double>()) {
    result.close();
    return;
  }

  // compute differences and average per second
  auto dt = current.get("time").getNumber<double>() -
            prev.get("time").getNumber<double>();

  if (dt <= 0) {
    result.close();
    return;
  }

  result.add("time", current.get("time"));

  VPackSlice currentSystem = current.get("system");
  VPackSlice prevSystem = prev.get("system");
  result.add("system", VPackValue(VPackValueType::Object));
  result.add(
      "minorPageFaultsPerSecond",
      VPackValue((currentSystem.get("minorPageFaults").getNumber<double>() -
                  prevSystem.get("minorPageFaults").getNumber<double>()) /
                 dt));
  result.add(
      "majorPageFaultsPerSecond",
      VPackValue((currentSystem.get("majorPageFaults").getNumber<double>() -
                  prevSystem.get("majorPageFaults").getNumber<double>()) /
                 dt));
  result.add("userTimePerSecond",
             VPackValue((currentSystem.get("userTime").getNumber<double>() -
                         prevSystem.get("userTime").getNumber<double>()) /
                        dt));
  result.add("systemTimePerSecond",
             VPackValue((currentSystem.get("systemTime").getNumber<double>() -
                         prevSystem.get("systemTime").getNumber<double>()) /
                        dt));
  result.add("residentSize", currentSystem.get("residentSize"));
  result.add("residentSizePercent", currentSystem.get("residentSizePercent"));
  result.add("virtualSize", currentSystem.get("virtualSize"));
  result.add("numberOfThreads", currentSystem.get("numberOfThreads"));
  result.close();

  // _serverStatistics()
  VPackSlice currentServer = current.get("server");
  result.add("server", VPackValue(VPackValueType::Object));
  result.add("physicalMemory", currentServer.get("physicalMemory"));
  result.add("uptime", currentServer.get("uptime"));

  VPackSlice currentV8Context = currentServer.get("v8Context");
  result.add("v8Context", VPackValue(VPackValueType::Object));
  if (currentV8Context.isObject()) {
    result.add("availablePerSecond", currentV8Context.get("available"));
    result.add("busyPerSecond", currentV8Context.get("busy"));
    result.add("dirtyPerSecond", currentV8Context.get("dirty"));
    result.add("freePerSecond", currentV8Context.get("free"));
    result.add("maxPerSecond", currentV8Context.get("max"));
  } else {
    // note: V8 may be turned off entirely on some servers
    result.add("availablePerSecond", VPackValue(0));
    result.add("busyPerSecond", VPackValue(0));
    result.add("dirtyPerSecond", VPackValue(0));
    result.add("freePerSecond", VPackValue(0));
    result.add("maxPerSecond", VPackValue(0));
  }
  result.close();

  VPackSlice currentThreads = currentServer.get("threads");
  result.add("threads", VPackValue(VPackValueType::Object));
  result.add("runningPerSecond", currentThreads.get("scheduler-threads"));
  result.add("workingPerSecond", currentThreads.get("in-progress"));
  result.add("blockedPerSecond", currentThreads.get("blocked"));
  result.add("queuedPerSecond", currentThreads.get("queued"));
  result.close();
  result.close();

  VPackSlice currentHttp = current.get("http");
  VPackSlice prevHttp = prev.get("http");
  result.add("http", VPackValue(VPackValueType::Object));
  result.add("requestsTotalPerSecond",
             VPackValue((currentHttp.get("requestsTotal").getNumber<double>() -
                         prevHttp.get("requestsTotal").getNumber<double>()) /
                        dt));
  result.add("requestsAsyncPerSecond",
             VPackValue((currentHttp.get("requestsAsync").getNumber<double>() -
                         prevHttp.get("requestsAsync").getNumber<double>()) /
                        dt));
  result.add("requestsGetPerSecond",
             VPackValue((currentHttp.get("requestsGet").getNumber<double>() -
                         prevHttp.get("requestsGet").getNumber<double>()) /
                        dt));
  result.add("requestsHeadPerSecond",
             VPackValue((currentHttp.get("requestsHead").getNumber<double>() -
                         prevHttp.get("requestsHead").getNumber<double>()) /
                        dt));
  result.add("requestsPostPerSecond",
             VPackValue((currentHttp.get("requestsPost").getNumber<double>() -
                         prevHttp.get("requestsPost").getNumber<double>()) /
                        dt));
  result.add("requestsPutPerSecond",
             VPackValue((currentHttp.get("requestsPut").getNumber<double>() -
                         prevHttp.get("requestsPut").getNumber<double>()) /
                        dt));
  result.add("requestsPatchPerSecond",
             VPackValue((currentHttp.get("requestsPatch").getNumber<double>() -
                         prevHttp.get("requestsPatch").getNumber<double>()) /
                        dt));
  result.add("requestsDeletePerSecond",
             VPackValue((currentHttp.get("requestsDelete").getNumber<double>() -
                         prevHttp.get("requestsDelete").getNumber<double>()) /
                        dt));
  result.add(
      "requestsOptionsPerSecond",
      VPackValue((currentHttp.get("requestsOptions").getNumber<double>() -
                  prevHttp.get("requestsOptions").getNumber<double>()) /
                 dt));
  result.add("requestsOtherPerSecond",
             VPackValue((currentHttp.get("requestsOther").getNumber<double>() -
                         prevHttp.get("requestsOther").getNumber<double>()) /
                        dt));
  result.close();

  VPackSlice currentClient = current.get("client");
  VPackSlice prevClient = prev.get("client");
  result.add("client", VPackValue(VPackValueType::Object));
  result.add("httpConnections", currentClient.get("httpConnections"));

  // bytes sent
  result.add(
      "bytesSentPerSecond",
      VPackValue(
          (currentClient.get("bytesSent").get("sum").getNumber<double>() -
           prevClient.get("bytesSent").get("sum").getNumber<double>()) /
          dt));

  result.add(VPackValue("bytesSentPercent"));
  avgPercentDistributon(result, currentClient.get("bytesSent"),
                        prevClient.get("bytesSent"), _bytesSentDistribution);

  // bytes received
  result.add(
      "bytesReceivedPerSecond",
      VPackValue(
          (currentClient.get("bytesReceived").get("sum").getNumber<double>() -
           prevClient.get("bytesReceived").get("sum").getNumber<double>()) /
          dt));

  result.add(VPackValue("bytesReceivedPercent"));
  avgPercentDistributon(result, currentClient.get("bytesReceived"),
                        prevClient.get("bytesReceived"),
                        _bytesReceivedDistribution);

  // total time
  auto d1 = currentClient.get("totalTime").get("count").getNumber<double>() -
            prevClient.get("totalTime").get("count").getNumber<double>();

  if (d1 == 0) {
    result.add("avgTotalTime", VPackValue(0));
  } else {
    result.add(
        "avgTotalTime",
        VPackValue(
            (currentClient.get("totalTime").get("sum").getNumber<double>() -
             prevClient.get("totalTime").get("sum").getNumber<double>()) /
            d1));
  }

  result.add(VPackValue("totalTimePercent"));
  avgPercentDistributon(result, currentClient.get("totalTime"),
                        prevClient.get("totalTime"), _requestTimeDistribution);

  // request time
  d1 = currentClient.get("requestTime").get("count").getNumber<double>() -
       prevClient.get("requestTime").get("count").getNumber<double>();

  if (d1 == 0) {
    result.add("avgRequestTime", VPackValue(0));
  } else {
    result.add(
        "avgRequestTime",
        VPackValue(
            (currentClient.get("requestTime").get("sum").getNumber<double>() -
             prevClient.get("requestTime").get("sum").getNumber<double>()) /
            d1));
  }

  result.add(VPackValue("requestTimePercent"));
  avgPercentDistributon(result, currentClient.get("requestTime"),
                        prevClient.get("requestTime"),
                        _requestTimeDistribution);

  // queue time
  d1 = currentClient.get("queueTime").get("count").getNumber<double>() -
       prevClient.get("queueTime").get("count").getNumber<double>();

  if (d1 == 0) {
    result.add("avgQueueTime", VPackValue(0));
  } else {
    result.add(
        "avgQueueTime",
        VPackValue(
            (currentClient.get("queueTime").get("sum").getNumber<double>() -
             prevClient.get("queueTime").get("sum").getNumber<double>()) /
            d1));
  }

  result.add(VPackValue("queueTimePercent"));
  avgPercentDistributon(result, currentClient.get("queueTime"),
                        prevClient.get("queueTime"), _requestTimeDistribution);

  // io time
  d1 = currentClient.get("ioTime").get("count").getNumber<double>() -
       prevClient.get("ioTime").get("count").getNumber<double>();

  if (d1 == 0) {
    result.add("avgIoTime", VPackValue(0));
  } else {
    result.add(
        "avgIoTime",
        VPackValue((currentClient.get("ioTime").get("sum").getNumber<double>() -
                    prevClient.get("ioTime").get("sum").getNumber<double>()) /
                   d1));
  }

  result.add(VPackValue("ioTimePercent"));
  avgPercentDistributon(result, currentClient.get("ioTime"),
                        prevClient.get("ioTime"), _requestTimeDistribution);

  result.close();

  if (!_clusterId.empty()) {
    result.add("clusterId", VPackValue(_clusterId));
  }

  result.close();
}

void StatisticsWorker::avgPercentDistributon(VPackBuilder& builder,
                                             VPackSlice now, VPackSlice last,
                                             VPackBuilder const& cuts) const {
  uint32_t n = static_cast<uint32_t>(cuts.slice().length() + 1);
  double count = 0;
  std::vector<double> result(n, 0);

  if (last.hasKey("count")) {
    count = now.get("count").getNumber<double>() -
            last.get("count").getNumber<double>();
  } else {
    count = now.get("count").getNumber<double>();
  }

  if (count > 0.0) {
    VPackSlice counts = now.get("counts");
    VPackSlice lastCounts = last.get("counts");
    for (uint32_t i = 0; i < n; i++) {
      result[i] = (counts.at(i).getNumber<double>() -
                   lastCounts.at(i).getNumber<double>()) /
                  count;
    }
  }

  builder.openObject();
  builder.add("values", VPackValue(VPackValueType::Array));
  for (auto const& i : result) {
    builder.add(VPackValue(i));
  }
  builder.close();

  builder.add("cuts", cuts.slice());

  builder.close();
}

void StatisticsWorker::generateRawStatistics(VPackBuilder& builder,
                                             double now) {
  ProcessInfo info = TRI_ProcessInfoSelf();
  uint64_t rss = static_cast<uint64_t>(info._residentSize);
  double rssp = 0;

  if (PhysicalMemory::getValue() != 0) {
    rssp = static_cast<double>(rss) /
           static_cast<double>(PhysicalMemory::getValue());
  }

  ConnectionStatistics::Snapshot connectionStats;
  ConnectionStatistics::getSnapshot(connectionStats);

  RequestStatistics::Snapshot requestStats;
  RequestStatistics::getSnapshot(requestStats,
                                 stats::RequestStatisticsSource::ALL);

  ServerStatistics const& serverInfo =
      _vocbase.server()
          .getFeature<metrics::MetricsFeature>()
          .serverStatistics();

  builder.openObject();
  if (!_clusterId.empty()) {
    builder.add("clusterId", VPackValue(_clusterId));
  }

  builder.add("time", VPackValue(now));

  // processStatistics()
  builder.add("system", VPackValue(VPackValueType::Object));
  builder.add("minorPageFaults", VPackValue(info._minorPageFaults));
  builder.add("majorPageFaults", VPackValue(info._majorPageFaults));
  if (info._scClkTck != 0) {
    // prevent division by zero
    builder.add("userTime", VPackValue(static_cast<double>(info._userTime) /
                                       static_cast<double>(info._scClkTck)));
    builder.add("systemTime", VPackValue(static_cast<double>(info._systemTime) /
                                         static_cast<double>(info._scClkTck)));
  }
  builder.add("numberOfThreads", VPackValue(info._numberThreads));
  builder.add("residentSize", VPackValue(rss));
  builder.add("residentSizePercent", VPackValue(rssp));
  builder.add("virtualSize", VPackValue(info._virtualSize));
  builder.close();

  // _clientStatistics()
  builder.add("client", VPackValue(VPackValueType::Object));
  builder.add("httpConnections",
              VPackValue(connectionStats.httpConnections.get()));

  VPackBuilder tmp =
      StatisticsFeature::fillDistribution(connectionStats.connectionTime);
  builder.add("connectionTime", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.totalTime);
  builder.add("totalTime", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.requestTime);
  builder.add("requestTime", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.queueTime);
  builder.add("queueTime", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.ioTime);
  builder.add("ioTime", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.bytesSent);
  builder.add("bytesSent", tmp.slice());

  tmp = StatisticsFeature::fillDistribution(requestStats.bytesReceived);
  builder.add("bytesReceived", tmp.slice());
  builder.close();

  // _httpStatistics()
  using rest::RequestType;
  builder.add("http", VPackValue(VPackValueType::Object));
  builder.add("requestsTotal", VPackValue(connectionStats.totalRequests.get()));
  builder.add("requestsSuperuser",
              VPackValue(connectionStats.totalRequestsSuperuser.get()));
  builder.add("requestsUser",
              VPackValue(connectionStats.totalRequestsUser.get()));
  builder.add("requestsAsync", VPackValue(connectionStats.asyncRequests.get()));
  builder.add(
      "requestsGet",
      VPackValue(connectionStats.methodRequests[(int)RequestType::GET].get()));
  builder.add(
      "requestsHead",
      VPackValue(connectionStats.methodRequests[(int)RequestType::HEAD].get()));
  builder.add(
      "requestsPost",
      VPackValue(connectionStats.methodRequests[(int)RequestType::POST].get()));
  builder.add(
      "requestsPut",
      VPackValue(connectionStats.methodRequests[(int)RequestType::PUT].get()));
  builder.add(
      "requestsPatch",
      VPackValue(
          connectionStats.methodRequests[(int)RequestType::PATCH].get()));
  builder.add(
      "requestsDelete",
      VPackValue(
          connectionStats.methodRequests[(int)RequestType::DELETE_REQ].get()));
  builder.add(
      "requestsOptions",
      VPackValue(
          connectionStats.methodRequests[(int)RequestType::OPTIONS].get()));
  builder.add(
      "requestsOther",
      VPackValue(
          connectionStats.methodRequests[(int)RequestType::ILLEGAL].get()));
  builder.close();

  // _serverStatistics()
  builder.add("server", VPackValue(VPackValueType::Object));
  builder.add("uptime", VPackValue(serverInfo.uptime()));
  builder.add("physicalMemory", VPackValue(PhysicalMemory::getValue()));
  builder.add("transactions", VPackValue(VPackValueType::Object));
  builder.add(
      "started",
      VPackValue(
          serverInfo._transactionsStatistics._transactionsStarted.load()));
  builder.add(
      "aborted",
      VPackValue(
          serverInfo._transactionsStatistics._transactionsAborted.load()));
  builder.add(
      "committed",
      VPackValue(
          serverInfo._transactionsStatistics._transactionsCommitted.load()));
  builder.add(
      "intermediateCommits",
      VPackValue(
          serverInfo._transactionsStatistics._intermediateCommits.load()));
  builder.add(
      "readOnly",
      VPackValue(serverInfo._transactionsStatistics._readTransactions.load()));
  builder.add(
      "dirtyReadOnly",
      VPackValue(
          serverInfo._transactionsStatistics._dirtyReadTransactions.load()));
  builder.close();

  // export v8 statistics
  builder.add("v8Context", VPackValue(VPackValueType::Object));
#ifdef USE_V8
  V8DealerFeature::Statistics v8Counters{};
  // V8 may be turned off on a server
  if (server().hasFeature<V8DealerFeature>()) {
    V8DealerFeature& dealer = server().getFeature<V8DealerFeature>();
    if (dealer.isEnabled()) {
      v8Counters = dealer.getCurrentExecutorStatistics();
    }
  }
  builder.add("available", VPackValue(v8Counters.available));
  builder.add("busy", VPackValue(v8Counters.busy));
  builder.add("dirty", VPackValue(v8Counters.dirty));
  builder.add("free", VPackValue(v8Counters.free));
  builder.add("min", VPackValue(v8Counters.min));
  builder.add("max", VPackValue(v8Counters.max));
#else
  builder.add("available", VPackValue(0));
  builder.add("busy", VPackValue(0));
  builder.add("dirty", VPackValue(0));
  builder.add("free", VPackValue(0));
  builder.add("min", VPackValue(0));
  builder.add("max", VPackValue(0));
#endif
  builder.close();

  // export threads statistics
  builder.add("threads", VPackValue(VPackValueType::Object));
  SchedulerFeature::SCHEDULER->toVelocyPack(builder);
  builder.close();

  // export ttl statistics
  TtlFeature& ttlFeature = server().getFeature<TtlFeature>();
  builder.add(VPackValue("ttl"));
  ttlFeature.statsToVelocyPack(builder);

  builder.close();

  builder.close();
}

void StatisticsWorker::saveSlice(VPackSlice slice,
                                 std::string const& collection) const {
  if (isStopping()) {
    return;
  }

  arangodb::OperationOptions opOptions;

  opOptions.waitForSync = false;
  opOptions.silent = true;

  // find and load collection given by name or identifier
  auto origin = transaction::OperationOriginInternal{"storing statistics data"};

  auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
  SingleCollectionTransaction trx(std::move(ctx), collection,
                                  AccessMode::Type::WRITE);

  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (!res.ok()) {
    if (!res.is(TRI_ERROR_SHUTTING_DOWN)) {
      LOG_TOPIC("ecdb9", WARN, Logger::STATISTICS)
          << "could not start transaction on " << collection << ": "
          << res.errorMessage();
    }
    return;
  }

  arangodb::OperationResult result = trx.insert(collection, slice, opOptions);

  // Will commit if no error occured.
  // or abort if an error occured.
  // result stays valid!
  res = trx.finish(result.result);
  if (res.fail() && res.isNot(TRI_ERROR_ARANGO_READ_ONLY)) {
    // BTS-1696: suppress log spam when we are in read-only mode.
    LOG_TOPIC("82af5", WARN, Logger::STATISTICS)
        << "could not commit stats to " << collection << ": "
        << res.errorMessage();
  }
}

void StatisticsWorker::beginShutdown() {
  Thread::beginShutdown();

  // wake up
  std::lock_guard guard{_cv.mutex};
  _cv.cv.notify_one();
}

void StatisticsWorker::run() {
  // statistics queries don't work on DB servers, so we should not
  // run the StatisticsWorker on DB servers!
  TRI_ASSERT(!ServerState::instance()->isDBServer());

  while (ServerState::isStartupOrMaintenance()) {
    if (isStopping()) {
      // startup aborted
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  try {
    if (ServerState::instance()->isRunningInCluster()) {
      // compute cluster id just once
      _clusterId = ServerState::instance()->getId();
    }
  } catch (...) {
    // do not fail hard here, as we are inside a thread!
  }

  // if we have multiple servers in the cluster, we don't want them
  // to execute their statistics insert requests all at the same time.
  // we use this term to add a bit of variance
  TRI_ASSERT(STATISTICS_INTERVAL > 1);
  uint64_t const ourTerm = RandomGenerator::interval(STATISTICS_INTERVAL - 1);
  TRI_ASSERT(ourTerm < STATISTICS_INTERVAL);

  uint64_t seconds = 0;
  while (!isStopping()) {
    TRI_IF_FAILURE("StatisticsWorker::bypass") {
      std::unique_lock guard{_cv.mutex};
      _cv.cv.wait_for(guard, std::chrono::seconds{1});
      continue;
    }

    seconds++;
    try {
      if (seconds % STATISTICS_INTERVAL == ourTerm) {
        // new stats are produced every 10 seconds
        historian();
      }

      if (seconds % GC_INTERVAL == 0) {
        // runs every 8 minutes
        collectGarbage();
      }

      if (seconds % HISTORY_INTERVAL == 0) {
        // process every 15 minutes
        historianAverage();
      }
    } catch (basics::Exception const& ex) {
      if (ex.code() != TRI_ERROR_ARANGO_READ_ONLY) {
        LOG_TOPIC("2fbcb", WARN, Logger::STATISTICS)
            << "caught exception in StatisticsWorker: " << ex.what();
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("92a40", WARN, Logger::STATISTICS)
          << "caught exception in StatisticsWorker: " << ex.what();
    } catch (...) {
      LOG_TOPIC("9a4f9", WARN, Logger::STATISTICS)
          << "caught unknown exception in StatisticsWorker";
    }

    std::unique_lock guard{_cv.mutex};
    _cv.cv.wait_for(guard, std::chrono::seconds{1});
  }
}
